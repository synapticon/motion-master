# Generator templates

`pnpm generate` builds `src/generated/` from `apps/motion_master/swagger.yml` with
swagger-typescript-api. The files in this folder change what it generates.

Without them, the generated client sends every body as JSON. It has no rule for a body of
`application/octet-stream` or `application/xml`, so `JSON.stringify` turns a file into `{}`. With
them, such a body goes as it is, so every operation can go through `client.api`, and its call returns
the response with its headers, such as `Server-Timing`.

| File | What it changes |
| --- | --- |
| `content-type-accessors.ejs` | Adds the content types `Xml` and `Binary`. A body that is not JSON, form data or text is `Binary`. |
| `http-client.ejs` | Adds `Xml` and `Binary` to `ContentType`, each with a formatter that sends the body as it is. |
| `procedure-call.ejs` | Sets the content type only on a call that has a body, so that a GET needs no CORS preflight. An XML body keeps `application/xml`. |
| `config.mjs` | Types a binary body as `Blob \| Uint8Array` in place of `File`. |

The three `.ejs` files are copies of the templates of swagger-typescript-api 13.12.2, with the
changes above. A template that is not here comes from the package. The version of the package is
pinned exactly in `package.json`, because a new version can change its templates. After an upgrade,
copy the three templates of the new version again and make the same changes.
