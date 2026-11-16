import { HttpResponse, http } from 'msw/http';
import { describe, expect, it } from 'vitest';
import { type AutoTuningRunRequest, HttpError, MotionMasterClient } from '../src/index';
import { HTTP_URL, server } from './fakes';

const RUN = `${HTTP_URL}/api/auto-tuning/run`;
const REQUEST: AutoTuningRunRequest = {
  run: 'auto_tune_velocity_controller',
  data: {
    zeta: 1.1,
    demanded_bw: 5,
    plant_model: { numerator: [1], denominator: [0.00015, 0.000206] },
  },
};

describe('auto-tuning', () => {
  it('sends the request as JSON and returns the result, also one with an error', async () => {
    const bodies: unknown[] = [];
    let answer: Record<string, unknown> = { gains: { velocity: { kp: 1 } }, duration: 0.2 };
    server.use(
      http.post(RUN, async ({ request }) => {
        bodies.push(await request.json());
        return HttpResponse.json(answer);
      }),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    expect(await client.runAutoTuning(REQUEST)).toEqual(answer);
    answer = { error: { message: 'A linear model cannot be fit.' } };
    expect(await client.runAutoTuning(REQUEST)).toEqual(answer);
    expect(bodies[0]).toEqual(REQUEST);
  });

  it('throws an HttpError whose response still holds the body', async () => {
    server.use(
      http.post(RUN, () =>
        HttpResponse.json({ error: 'auto-tuning is not running' }, { status: 503 }),
      ),
    );
    const client = new MotionMasterClient({ baseUrl: HTTP_URL });
    const error = await client.runAutoTuning(REQUEST).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(HttpError);
    expect((error as HttpError).reason).toBe('auto-tuning is not running');
    expect(await (error as HttpError).response.json()).toEqual({
      error: 'auto-tuning is not running',
    });
  });
});
