import { describe, expect, it } from 'vitest';
import { fakeServer } from './fakes';

const RESULT = {
  networkAdapter: 'spoe0',
  durationMs: 2000,
  answerDelayMs: 1500,
  devices: [
    {
      ipAddress: '192.168.100.17',
      macAddress: '40:49:8a:01:29:3c',
      name: 'Actilink',
      firmware: 'ethernet-ip',
      vendorId: 1783,
      deviceId: null,
      productCode: 1025,
      serialNumber: 1938,
      sameSubnet: true,
      foundBy: ['list-identity'],
    },
  ],
  warnings: [],
};

describe('discoverEthernetDevices', () => {
  it('sends the network adapter and, when given, the time, and returns the result', async () => {
    const server = fakeServer({
      'GET /api/ethernet/discovery': () => [200, RESULT],
    });
    expect(await server.client.discoverEthernetDevices('spoe0')).toEqual(RESULT);
    await server.client.discoverEthernetDevices('spoe0', { durationMs: 35000 });
    expect(server.calls()).toEqual([
      'GET /api/ethernet/discovery?networkAdapter=spoe0',
      'GET /api/ethernet/discovery?networkAdapter=spoe0&durationMs=35000',
    ]);
  });

  it('rejects with the reason of the server', async () => {
    const server = fakeServer({
      'GET /api/ethernet/discovery': () => [404, { error: "network adapter 'x' not found" }],
    });
    await expect(server.client.discoverEthernetDevices('x')).rejects.toThrow('not found');
  });
});
