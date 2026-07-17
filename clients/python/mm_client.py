"""A tiny Motion Master HTTP API client driven by the OpenAPI spec.

On construction the client fetches the running server's own OpenAPI spec from
GET /api/swagger.yml, indexes every operation by its operationId, and resolves
the HTTP method + URL path template from that spec at call time. Example scripts
therefore never hardcode a URL or verb -- they name the operationId straight out
of the spec the server itself is serving:

    client.call("init", body={"driver": "soem", "adapter": "eth0"})
    client.call("sdoUpload", path={"slavePosition": 1, "index": 0x6041, "subindex": 0})

Reading the spec from the live server (rather than a file in this repo) means
the client is standalone -- no source checkout needed -- and always matches the
exact contract of the server it is talking to. The spec is parsed with PyYAML:
we only need operationId -> method + path, so a generic YAML load is enough (and
avoids a strict OpenAPI validator rejecting perfectly legal media types such as
the text/yaml this very endpoint declares).
"""

from __future__ import annotations

import json
import ssl

import requests
import urllib3
import yaml

# OpenAPI path-item keys that denote an operation (as opposed to parameters,
# summary, etc.); each maps operationId -> (method, path template).
_HTTP_METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}


class MMClient:
    """Thin wrapper over requests that resolves operations from the server's OpenAPI spec."""

    def __init__(self, http_url, ws_url=None, verify=False):
        self._http_url = http_url.rstrip("/")
        # The monitoring WebSocket endpoint. Held here so the client is the one
        # connection object -- a future monitor()/trace-process-data method reads
        # this instead of callers wiring up a second object.
        self.ws_url = ws_url
        self._verify = verify
        self._session = requests.Session()

        # A self-signed dev cert (verify=False) would otherwise spam a warning
        # on every request and drown the example output.
        if verify is False:
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

        # Fetch and parse the server's own spec: operationId -> (http method,
        # path template such as "/api/devices/{slavePosition}/sdo/{index}/{subindex}").
        response = self._session.get(f"{self._http_url}/api/swagger.yml", verify=verify)
        response.raise_for_status()
        spec = yaml.safe_load(response.text)

        self._ops = {}
        for template, item in spec.get("paths", {}).items():
            for method, op in item.items():
                if method in _HTTP_METHODS and isinstance(op, dict) and "operationId" in op:
                    self._ops[op["operationId"]] = (method.upper(), template)

    def call(self, operation_id, path=None, body=None, query=None, check=True, **kwargs):
        """Invoke an operation by its operationId.

        path/body/query map to URL path params, the JSON request body, and the
        query string. Extra kwargs are forwarded to requests (e.g. data=... for
        the raw octet-stream FoE upload). Set check=False to inspect non-2xx
        responses yourself (e.g. init's 409 "already initialised").
        """
        if operation_id not in self._ops:
            raise KeyError(f"unknown operationId {operation_id!r} (not in the OpenAPI spec)")

        method, template = self._ops[operation_id]
        url = self._http_url + template.format(**(path or {}))

        response = self._session.request(
            method, url, json=body, params=query, verify=self._verify, **kwargs
        )
        if check:
            response.raise_for_status()
        return response

    def _ssl_context(self):
        """Build an SSL context matching the HTTP verify setting for the WebSocket."""
        if self._verify is False:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return ctx
        if isinstance(self._verify, str):
            return ssl.create_default_context(cafile=self._verify)
        return ssl.create_default_context()

    def monitor(self, topic):
        """Subscribe to a monitoring topic and yield its batches from the WebSocket.

        The monitoring itself must already exist (POST /api/monitorings, i.e.
        client.call("createMonitoring", ...)). This opens the WebSocket, sends
        {"subscribe": topic}, and yields each batch's rows -- a list of
        [timestampUs, v0, v1, ...] cycle rows, positionally ordered by the
        monitoring's parameters. The stream is lossless: every recorded cycle
        since the last flush arrives in the batch. The caller stops by breaking
        out of the loop; the socket and subscription are torn down on exit.
        """
        if not self.ws_url:
            raise RuntimeError("no ws_url configured on this client")

        # Lazy import so HTTP-only usage doesn't require the websockets package.
        from websockets.sync.client import connect

        with connect(self.ws_url, ssl=self._ssl_context()) as ws:
            ws.send(json.dumps({"subscribe": topic}))
            try:
                for message in ws:
                    msg = json.loads(message)
                    if msg.get("type") == "monitoring" and msg.get("topic") == topic:
                        yield msg["data"]
            finally:
                # Best-effort unsubscribe; the socket close below also ends it.
                try:
                    ws.send(json.dumps({"unsubscribe": topic}))
                except Exception:
                    pass
