"""AL state changes of the Integro, whose netX and SoC each keep their own AL state."""

from __future__ import annotations

import contextlib
import time

import pytest
from motion_master_client import AlState, Cia402, OperationMode, Profile

from bench import Bench
from ethercat_pcap import foe_errors

# The target velocity of the drop test, in rpm. Low, because the motor coasts down after the drop.
DROP_RPM = 200
# The drive must turn at least this fast before the drop, so that the drop is one while turning.
TURNING_RPM = DROP_RPM // 2
RUN_SECONDS = 1.0
# The time after the drop before the test reads the state of the drive. A cyclic timeout that
# latches a fault does so within it.
AFTER_DROP_SECONDS = 1.0

# A refused or lost request ends at the mailbox timeout of the server, about 700 ms. An answer
# comes much sooner.
MAX_ANSWER_SECONDS = 0.1
# The Integro answered no SDO request after about 150 requests that got no answer.
REPEATED_REQUESTS = 200

# A file that no SOMANET drive holds.
MISSING_FILE = "bench-missing-file.txt"
FOE_NOT_FOUND = 0x8001
# The read timeout of the server for FoE, so the refusal of a read must come sooner.
MAX_FOE_READ_SECONDS = 6.0

# The Timestamp Object, which the EtherCAT stack of an Integro created before V5.5.0.0 of the stack.
TIMESTAMP_OBJECT = 0x10F8

# (index, subindex, the SDO abort code the Integro must answer with, or None for a value).
MISSING_ENTRY_REQUESTS = [
    (0x5555, 0x00, 0x06020000),  # an index the drive does not have
    (0xF050, 0x02, 0x06020000),  # an index the dictionary does not list
    (0x6041, 0x05, 0x06090011),  # a subindex the object does not have
    (0x1601, 0x03, None),  # a slot that a PDO mapping object reserves
]


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


def _change_state(bench: Bench, state: AlState, position: int) -> None:
    result = bench.client.call(
        "changeState", body={"state": state, "positions": [position]}
    ).json()
    assert result["ok"], result


@pytest.mark.parametrize("drop_to", [AlState.SAFEOP, AlState.INIT], ids=["safe_op", "init"])
def test_integro_ends_without_fault_after_a_drop_while_turning(
    bench: Bench, record_property, drop_to: AlState
):
    # The netX of an Integro did not tell its SoC of a drop from OP. The SoC still counted itself
    # in OP, got no outputs, and its cyclic timeout latched CyclicHb (0x7500). After the drop the
    # drive must end in Switch on disabled, with no fault.
    integro = bench.device("integro")
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)
    client.restart(AlState.OP, positions=[integro.position])

    try:
        client.set_operation_mode(integro.position, OperationMode.CYCLIC_SYNC_VELOCITY)
        # A stale target velocity would start the motor at the enable.
        client.set_target(integro.position, "velocity", 0)
        client.enable(integro.position)
        client.set_target(integro.position, "velocity", DROP_RPM)
        time.sleep(RUN_SECONDS)
        velocity = client.read_value(integro.position, Cia402.VELOCITY_ACTUAL_VALUE)
        assert velocity >= TURNING_RPM, f"the motor turns at {velocity} rpm before the drop"

        _change_state(bench, drop_to, integro.position)
        time.sleep(AFTER_DROP_SECONDS)
        if drop_to == AlState.INIT:
            # INIT has no mailbox, so the drive goes to PREOP for the reads.
            _change_state(bench, AlState.PREOP, integro.position)
        statusword = client.read_value(integro.position, Cia402.STATUSWORD)
        error_code = client.read_value(integro.position, Cia402.ERROR_CODE)
    finally:
        # A failed step here must not hide the error of the test.
        with contextlib.suppress(Exception):
            client.restart(AlState.INIT, positions=[integro.position])

    state = Cia402.state(statusword)
    assert state == "SwitchOnDisabled", f"the drive is in {state}, 0x603F is 0x{error_code:04X}"
    assert error_code == 0, f"0x603F is 0x{error_code:04X}"
    record_property(
        "message",
        f"device {integro.position}: {velocity} rpm in CSV, OP to {drop_to.name}: {state},"
        f" 0x603F 0x{error_code:04X}",
    )


def _sdo_upload(bench: Bench, position: int, index: int, subindex: int):
    """Upload index:subindex, and return the response and the time it took in seconds."""
    start = time.monotonic()
    response = bench.client.call(
        "sdoUpload",
        path={"slavePosition": position, "index": index, "subindex": subindex},
        check=False,
    )
    return response, time.monotonic() - start


def _check_answer(response, seconds: float, index: int, subindex: int, abort: int | None) -> str:
    """Check one answer against the expected abort code, or a value, and return what it was."""
    name = f"0x{index:04X}:{subindex:02X}"
    assert seconds <= MAX_ANSWER_SECONDS, f"{name} took {seconds:.3f} s: {response.text}"
    if abort is None:
        assert response.status_code == 200, f"{name}: {response.text}"
        return f"{name} value {response.json()['data']}"
    assert response.status_code != 200, f"{name} answered a value: {response.text}"
    code = f"0x{abort:08X}"
    assert f"SDO abort {code}" in response.json()["error"], f"{name}: {response.text}"
    return f"{name} abort {code}"


def test_integro_answers_requests_for_missing_entries(bench: Bench, record_property):
    # The netX of an Integro sent no answer, or a wrong one, to a request for an object or subindex
    # it does not have. Each such request waited for the mailbox timeout, and after about 150 of
    # them the drive answered no SDO request until a power cycle.
    integro = bench.device("integro")
    bench.client.reset()
    bench.client.init()
    bench.client.restart(AlState.PREOP)

    answers = []
    for index, subindex, abort in MISSING_ENTRY_REQUESTS:
        response, seconds = _sdo_upload(bench, integro.position, index, subindex)
        answers.append(_check_answer(response, seconds, index, subindex, abort))

    index, subindex, abort = MISSING_ENTRY_REQUESTS[1]
    slowest = 0.0
    for n in range(REPEATED_REQUESTS):
        response, seconds = _sdo_upload(bench, integro.position, index, subindex)
        try:
            _check_answer(response, seconds, index, subindex, abort)
        except AssertionError as error:
            raise AssertionError(f"request {n + 1} of {REPEATED_REQUESTS}: {error}") from None
        slowest = max(slowest, seconds)
    response, seconds = _sdo_upload(bench, integro.position, 0x1000, 0x00)
    after = _check_answer(response, seconds, 0x1000, 0x00, None)

    record_property(
        "message",
        f"device {integro.position}: {'; '.join(answers)}; {REPEATED_REQUESTS} requests for"
        f" 0x{index:04X}:{subindex:02X} each aborted, the slowest in {slowest * 1000:.1f} ms;"
        f" then {after}",
    )


def test_integro_refuses_a_missing_file_without_leftover_bytes(bench: Bench, record_property):
    # The netX of an Integro answered an FoE read of a missing file with error 0x8001, and with up
    # to about 115 bytes of an earlier packet as the error text, the password and the file name of
    # the request among them. The server keeps only the code, so the test records the frames and
    # reads the text from them. A short text that names the error is allowed.
    integro = bench.device("integro")
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)

    client.start_capture(network_adapter=bench.description.network_adapter)
    try:
        start = time.monotonic()
        response = client.call(
            "foeReadFile",
            path={"slavePosition": integro.position, "filename": MISSING_FILE},
            check=False,
        )
        seconds = time.monotonic() - start
    finally:
        stopped = client.stop_capture()
        pcap = client.read_user_cache_file(stopped["path"])
        client.remove_user_cache_file(stopped["path"])

    assert response.status_code == 404, response.text
    assert f"0x{FOE_NOT_FOUND:04X}" in response.json()["error"], response.text
    assert seconds <= MAX_FOE_READ_SECONDS, f"the refusal took {seconds:.1f} s"
    errors = [e for e in foe_errors(pcap) if e.code == FOE_NOT_FOUND]
    assert errors, "the capture holds no FoE error 0x8001 from a slave"
    # The text may name the error, as "Path not found" does. It must not carry bytes of the
    # request, whose file name is the part that can be found.
    leaks = [e.text for e in errors if MISSING_FILE.encode() in e.text]
    assert not leaks, (
        f"{len(leaks)} of {len(errors)} FoE error packets carry the file name: {leaks}"
    )
    texts = sorted({e.text.decode("ascii", "backslashreplace") for e in errors})
    hardware_description = client.read_file(integro.position, ".hardware_description")
    assert hardware_description, "the drive sent an empty .hardware_description"

    record_property(
        "message",
        f"device {integro.position}: FoE read of {MISSING_FILE} answered 404 in"
        f" {seconds * 1000:.0f} ms; {len(errors)} FoE error 0x8001 packet(s) in the capture, with"
        f" the text(s) {texts} and no file name; then .hardware_description read"
        f" {len(hardware_description)} bytes",
    )


def test_integro_object_dictionary_matches_the_stack(bench: Bench, record_property):
    # The EtherCAT stack on the netX of an Integro created 0x10F8 in its own dictionary, which the
    # SoC dictionary does not hold. The firmware now runs a stack that leaves it out. The server
    # caches a dictionary only when it lost no entry, and the server of a bench run starts with an
    # empty cache.
    integro = bench.device("integro")
    client = bench.client
    client.reset()
    client.init()
    client.restart(AlState.PREOP)

    response, seconds = _sdo_upload(bench, integro.position, TIMESTAMP_OBJECT, 0x00)
    upload = _check_answer(response, seconds, TIMESTAMP_OBJECT, 0x00, 0x06020000)

    parameters = client.parameters(integro.position)
    addresses = {(p["index"], p["subindex"]) for p in parameters}
    timestamp = sorted(a for a in addresses if a[0] == TIMESTAMP_OBJECT)
    assert not timestamp, f"the dictionary holds 0x10F8: {timestamp}"

    device = client.devices()[integro.position - 1]
    version = client.read_value(integro.position, Profile.MANUFACTURER_SOFTWARE_VERSION)
    cached = [
        e
        for e in client.call("listParameterCacheEntries").json()
        if e["vendorId"] == device["vendorId"]
        and e["productCode"] == device["productCode"]
        and e["firmwareVersion"] == version
    ]
    assert cached, f"no parameter cache for the Integro with {version}, so an entry was lost"

    record_property(
        "message",
        f"device {integro.position}: {upload}; {len(parameters)} dictionary entries, none of"
        f" 0x10F8; cached with {cached[0]['parameterCount']} entries for {version}",
    )
