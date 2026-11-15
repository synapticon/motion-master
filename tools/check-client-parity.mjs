// Compares the public names of the Python and the TypeScript client with the parity table,
// clients/PARITY.md, and exits with 1 when they differ.
//
// The Python names come from tools/client_public_names.py, which reads the syntax tree of the
// client. The TypeScript names come from the TypeScript compiler, which reads src/index.ts of the
// client package. So the check needs python3 and the dependencies of the workspace, and no build.
//
//   node tools/check-client-parity.mjs

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const tablePath = join(root, 'clients', 'PARITY.md');
const packageDir = join(root, 'web', 'packages', 'motion-master-client');

// The compiler is a dependency of the client package, not of the repository root.
const ts = createRequire(join(packageDir, 'package.json'))('typescript');

const NONE = '—';
const MODULE = 'Module';

/** Parses the table into rows, each with its section: the module, or the name of a class. */
function readTable() {
  const rows = [];
  let section = null;
  for (const [lineIndex, line] of readFileSync(tablePath, 'utf8').split('\n').entries()) {
    const heading = /^## (.+)$/.exec(line);
    if (heading) {
      section = heading[1].trim();
      continue;
    }
    if (section === null || !line.startsWith('|')) {
      continue;
    }
    const cells = line
      .split('|')
      .slice(1, -1)
      .map((cell) => cell.trim());
    if (cells[0] === 'Python' || /^-+$/.test(cells[0])) {
      continue;
    }
    const name = (cell) => (cell === NONE ? null : cell.replace(/^`|`$/g, ''));
    rows.push({
      line: lineIndex + 1,
      section,
      python: name(cells[0]),
      typescript: name(cells[1]),
      pending: cells[2] === 'pending',
      status: cells[2],
      reason: cells[3],
    });
  }
  return rows;
}

/** Returns the TypeScript name of a Python name: a snake_case name in camelCase, any other as is. */
function camelCase(name) {
  if (!/^[a-z][a-z0-9_]*$/.test(name)) {
    return name;
  }
  return name.replace(/_([a-z0-9])/g, (_, letter) => letter.toUpperCase());
}

function readPythonNames() {
  const script = join(root, 'tools', 'client_public_names.py');
  return JSON.parse(execFileSync('python3', [script], { encoding: 'utf8' }));
}

function isPublicMember(member) {
  const flags = ts.getCombinedModifierFlags(member);
  // A static method counts, because Python has the class method from_config.
  if (flags & (ts.ModifierFlags.Private | ts.ModifierFlags.Protected)) {
    return false;
  }
  if (!member.name || ts.isPrivateIdentifier(member.name)) {
    return false;
  }
  return ts.isMethodDeclaration(member) || ts.isGetAccessorDeclaration(member);
}

/**
 * Returns the exports of the package: `values`, the runtime exports, which the table must list, and
 * `all`, which adds the type exports, so a table row may name a type. `classes` maps each exported
 * class to its public methods and getters. The generated API client is left out.
 */
function readTypeScriptNames() {
  const configPath = join(packageDir, 'tsconfig.json');
  const config = ts.readConfigFile(configPath, ts.sys.readFile);
  const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, packageDir);
  const entry = join(packageDir, 'src', 'index.ts');
  const program = ts.createProgram([entry], parsed.options);
  const checker = program.getTypeChecker();
  const moduleSymbol = checker.getSymbolAtLocation(program.getSourceFile(entry));

  const values = new Set();
  const all = new Set();
  const classes = {};
  for (const exported of checker.getExportsOfModule(moduleSymbol)) {
    const symbol =
      exported.flags & ts.SymbolFlags.Alias ? checker.getAliasedSymbol(exported) : exported;
    const declarations = symbol.declarations ?? [];
    // The compiler writes every file name with forward slashes, also on Windows.
    const fromGenerated = declarations.some((d) =>
      d.getSourceFile().fileName.includes('/src/generated/'),
    );
    if (fromGenerated) {
      continue;
    }
    const name = exported.name;
    all.add(name);
    if (symbol.flags & ts.SymbolFlags.Value) {
      values.add(name);
    }
    const classDeclaration = declarations.find(ts.isClassDeclaration);
    if (classDeclaration) {
      classes[name] = classDeclaration.members
        .filter(isPublicMember)
        .map((member) => member.name.getText());
    }
  }
  return { values: [...values], all: [...all], classes };
}

function check() {
  const rows = readTable();
  const errors = [];
  const fail = (row, message) => errors.push(`clients/PARITY.md:${row.line}: ${message}`);

  // The shape of each row.
  for (const row of rows) {
    if (row.status !== '' && !row.pending) {
      fail(row, `the status is "${row.status}", and only "pending" or nothing is allowed`);
    }
    if (row.python === null && row.typescript === null) {
      fail(row, 'the row names no helper');
    } else if ((row.python === null || row.typescript === null) && !row.pending && !row.reason) {
      fail(
        row,
        `${row.python ?? row.typescript} is in one client only, and the row gives no reason`,
      );
    } else if (row.python && row.typescript && !row.pending) {
      if (camelCase(row.python) !== row.typescript) {
        fail(
          row,
          `${row.python} must be ${camelCase(row.python)} in TypeScript, not ${row.typescript}`,
        );
      }
      if (row.reason) {
        fail(row, `${row.python} is in both clients, so the row needs no reason`);
      }
    }
  }

  const python = readPythonNames();
  const typescript = readTypeScriptNames();
  const sections = [...new Set(rows.map((row) => row.section))];
  for (const section of sections) {
    const sectionRows = rows.filter((row) => row.section === section);
    const inCode = {
      python: new Set(section === MODULE ? python.module : (python.classes[section] ?? [])),
      typescript: new Set(
        section === MODULE ? typescript.values : (typescript.classes[section] ?? []),
      ),
    };
    // A row may name a TypeScript type, such as Cia402State, but the table must list only values.
    const existing = {
      python: inCode.python,
      typescript: section === MODULE ? new Set(typescript.all) : inCode.typescript,
    };
    for (const language of ['python', 'typescript']) {
      const listed = new Set(sectionRows.map((row) => row[language]).filter(Boolean));
      for (const name of inCode[language]) {
        if (!listed.has(name)) {
          errors.push(
            `clients/PARITY.md, ${section}: the ${language} client has ${name}, and the table does not list it`,
          );
        }
      }
      for (const row of sectionRows) {
        const name = row[language];
        if (name && !row.pending && !existing[language].has(name)) {
          fail(row, `the ${language} client has no ${name}`);
        }
      }
    }
  }
  const pending = rows.filter((row) => row.pending).length;
  if (errors.length > 0) {
    for (const error of errors) {
      console.error(error);
    }
    process.exit(1);
  }
  console.log(`The clients match clients/PARITY.md: ${rows.length} rows, ${pending} pending.`);
}

check();
