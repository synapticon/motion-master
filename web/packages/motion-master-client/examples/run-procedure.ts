// List the procedures of a device, describe one, or run one by name with its parameters.
//
// With no name, the program lists every procedure the device has (GET .../procedures). With a name
// and --describe, it prints what the procedure does, its caveats and its parameters. With a name, it
// runs the procedure (POST .../procedures/{name}), prints each step when its status changes, and
// exits with 1 unless the run succeeds. Ctrl-C cancels the run.
//
// Parameters go on the command line as name=value. The program converts each value by the type the
// descriptor of the procedure gives:
//
//     integer       decimal or 0x-prefixed hex
//     boolean       true or false
//     enum          one of the option values that --describe lists
//     byteArray     bytes separated by commas, each decimal or 0x-prefixed hex
//     string        as written
//     stringArray   values separated by commas
//     file          a local path, which the program reads and sends as base64
//
// Opens the fieldbus driver, scans and moves the device to --state first when needed. The default is
// PREOP, because most procedures need the mailbox. A procedure that enables the drive needs OP,
// because the drive enables only while it exchanges process data. Give --state op for it.
//
//     pnpm example examples/run-procedure.ts
//     pnpm example examples/run-procedure.ts store-parameters --describe
//     pnpm example examples/run-procedure.ts restore-default-parameters group=application
//     pnpm example examples/run-procedure.ts os-command command=0x15,0,0,0,0,0,0,0 fsBuffer=read
//     pnpm example examples/run-procedure.ts offset-detection --state op --slave 2

import { readFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import {
  AlState,
  formatStep,
  MotionMasterClient,
  type ProcedureRequest,
  type ProcedureSnapshot,
  run,
  stepChanges,
  type TargetAlState,
} from '@synapticon/motion-master-client';

type Descriptor = Awaited<
  ReturnType<MotionMasterClient['api']['listProcedures']>
>['data'][number]['descriptor'];
type Parameter = Descriptor['parameters'][number];

// The states a device can be commanded to, by their names in lowercase: init, preop, boot, ...
const STATES = Object.fromEntries(
  Object.entries(AlState)
    .filter(([name]) => name !== 'UNKNOWN')
    .map(([name, value]) => [name.toLowerCase(), value]),
) as Record<string, TargetAlState>;

/** Prints `message` on stderr and ends the program with 1. */
function fail(message: string): never {
  console.error(message);
  process.exit(1);
}

/** Converts the text of one name=value argument to the JSON value of its parameter type. */
function convert(param: Parameter, text: string): unknown {
  switch (param.type) {
    case 'integer':
      return parseInteger(text);
    case 'boolean':
      if (!['true', 'false'].includes(text.toLowerCase())) {
        throw new Error('expected true or false');
      }
      return text.toLowerCase() === 'true';
    case 'enum': {
      const option = (param.options ?? []).find((o) => String(o.value) === text);
      if (option === undefined) {
        const values = (param.options ?? []).map((o) => String(o.value)).join(', ');
        throw new Error(`expected one of ${values}`);
      }
      return option.value;
    }
    case 'byteArray':
      return text.split(',').map(parseInteger);
    case 'stringArray':
      return text ? text.split(',') : [];
    case 'file':
      return readFileSync(text).toString('base64');
    default:
      return text;
  }
}

/** Parses a decimal or a 0x-prefixed hex integer. */
function parseInteger(text: string): number {
  const value = Number(text);
  if (!Number.isInteger(value)) {
    throw new Error(`${text} is not an integer`);
  }
  return value;
}

/** Builds the request body from name=value arguments, checked against the descriptor. */
function buildParams(descriptor: Descriptor, assignments: string[]): ProcedureRequest {
  const params = new Map(descriptor.parameters.map((p) => [p.name, p]));
  const body: Record<string, unknown> = {};
  for (const assignment of assignments) {
    const sep = assignment.indexOf('=');
    if (sep < 0) {
      fail(`'${assignment}': write a parameter as name=value`);
    }
    const name = assignment.slice(0, sep);
    const param = params.get(name);
    if (param === undefined) {
      const known = [...params.keys()].join(', ') || 'none';
      fail(`${descriptor.name} has no parameter '${name}' (parameters: ${known})`);
    }
    try {
      body[name] = convert(param, assignment.slice(sep + 1));
    } catch (e) {
      fail(`${name}: ${(e as Error).message}`);
    }
  }
  const missing = [...params.values()].filter((p) => p.required && !(p.name in body));
  if (missing.length > 0) {
    fail(`${descriptor.name} needs ${missing.map((p) => p.name).join(', ')}`);
  }
  return body as ProcedureRequest;
}

/** Returns the flags of a procedure in parentheses, worded as the Console shows them. */
function flags(descriptor: Descriptor): string {
  const words: string[] = [];
  if (descriptor.movesMotor) {
    words.push('can move the motor');
  }
  if (descriptor.requiresEnabled) {
    words.push('requires the drive enabled');
  }
  return words.length > 0 ? ` (${words.join(', ')})` : '';
}

/** Prints what a procedure does, its caveats and its parameters. */
function describe(descriptor: Descriptor): void {
  console.log(`${descriptor.name}: ${descriptor.title}${flags(descriptor)}`);
  console.log(`\n${descriptor.description}`);
  for (const caveat of descriptor.caveats) {
    console.log(`\n- ${caveat}`);
  }
  if (descriptor.parameters.length > 0) {
    console.log('\nparameters:');
  }
  for (const p of descriptor.parameters) {
    let spec = p.required ? 'required' : `default ${JSON.stringify(p.defaultValue ?? null)}`;
    if (p.type === 'enum') {
      spec += '; ' + (p.options ?? []).map((o) => `${String(o.value)} (${o.title})`).join(', ');
    }
    console.log(`  ${p.name} (${p.type}, ${spec})`);
    console.log(`      ${p.description}`);
  }
}

async function main(): Promise<void> {
  const { values: args, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      slave: { type: 'string', default: '1' },
      state: { type: 'string', default: 'preop' },
      describe: { type: 'boolean', default: false },
    },
  });
  const [name, ...assignments] = positionals;
  const slave = Number(args.slave);
  const state =
    STATES[args.state] ?? fail(`--state must be one of ${Object.keys(STATES).join(', ')}`);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(state, { positions: [slave] });

  const procedures = (await client.api.listProcedures(slave)).data;
  const descriptors = new Map(procedures.map((p) => [p.descriptor.name, p.descriptor]));

  if (name === undefined) {
    for (const d of descriptors.values()) {
      console.log(`  ${d.name.padEnd(32)} ${d.title}${flags(d)}`);
    }
    return;
  }

  const descriptor = descriptors.get(name) ?? fail(`slave ${slave} has no procedure '${name}'`);
  if (args.describe) {
    describe(descriptor);
    return;
  }

  const params = buildParams(descriptor, assignments);

  // Ctrl-C aborts the signal, so that runProcedure() cancels the run before the program ends.
  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort(new Error('cancelled')));

  let snapshot: ProcedureSnapshot | undefined;
  try {
    for await (const [current, changed] of stepChanges(
      client.runProcedure(slave, name, { params, signal: controller.signal }),
    )) {
      snapshot = current;
      for (const step of changed) {
        console.log(formatStep(step));
      }
    }
  } catch (e) {
    if (controller.signal.aborted) {
      console.error('\ncancelled');
      process.exitCode = 1;
      return;
    }
    throw e;
  }

  if (snapshot?.error) {
    console.log(`error: ${snapshot.error}`);
  }
  console.log(`${name} ${snapshot?.status}`);
  if (snapshot?.status !== 'succeeded') {
    process.exitCode = 1;
  }
}

await run(main);
