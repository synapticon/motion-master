"""AL state changes of the Integro, whose netX and SoC each keep their own AL state."""

from __future__ import annotations

from motion_master_client import AlState

from bench import Bench


def _mapping_request(mapping: dict) -> dict:
    """Return a PDO mapping as read (getDevicePdoMapping) in the shape of a write request.

    The read gives each entry its bit offset, which the device derives, so the request omits it.
    """

    def objects(direction: str) -> list[dict]:
        return [
            {
                "pdoIndex": o["pdoIndex"],
                "entries": [
                    {"index": e["index"], "subindex": e["subindex"], "bitLength": e["bitLength"]}
                    for e in o["entries"]
                ],
            }
            for o in mapping[direction]
        ]

    return {"outputs": objects("outputs"), "inputs": objects("inputs")}


def test_integro_takes_a_pdo_mapping_write_at_pre_op_after_safe_op(
    bench: Bench, record_property
):
    # The netX of an Integro did not tell its SoC of a step down from SAFEOP. Back at PREOP the
    # SoC still applied the rules of SAFEOP, and refused a write of an object that is writable at
    # PREOP only, such as the PDO assignment. The test steps down directly, not through INIT,
    # because INIT is what told the SoC.
    integro = bench.device("integro")
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.SAFEOP)
    result = bench.client.call(
        "changeState", body={"state": AlState.PREOP, "positions": [integro.position]}
    ).json()
    assert result["ok"], result

    mapping = bench.client.call("getDevicePdoMapping", path=integro.path).json()
    request = _mapping_request(mapping)
    write = bench.client.call(
        "writeDevicePdoMapping", path=integro.path, body=request, check=False
    )
    assert write.status_code == 200, write.text
    assert _mapping_request(write.json()) == request, write.json()

    # The written mapping must also bring the drive back to SAFEOP.
    result = bench.client.call(
        "changeState", body={"state": AlState.SAFEOP, "positions": [integro.position]}
    ).json()
    assert result["ok"], result
    states = bench.client.call("getDeviceStates").json()
    assert states[integro.position - 1]["alState"] == AlState.SAFEOP, states
    outputs = sum(len(o["entries"]) for o in request["outputs"])
    inputs = sum(len(o["entries"]) for o in request["inputs"])
    record_property(
        "message",
        f"device {integro.position}: SAFEOP to PREOP, the PDO mapping write of"
        f" {len(request['outputs'])} RxPDO ({outputs} entries) and {len(request['inputs'])} TxPDO"
        f" ({inputs} entries) answered 200, and the drive went back to SAFEOP",
    )
