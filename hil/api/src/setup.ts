import { logFetch } from './log-fetch.js';
import { Api } from './mm-api.js';

export const api = new Api({
  baseUrl: process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:8443',
  customFetch: (input, init) => logFetch('req', input, init),
});
