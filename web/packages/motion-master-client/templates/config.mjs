// The custom config of swagger-typescript-api for the generated client. A binary body, a string of
// format binary in swagger.yml, is a Blob or a Uint8Array, which fetch sends as it is. The generator
// types it as File by default, which a Uint8Array is not.
export default {
  primitiveTypeConstructs: (struct) => ({
    ...struct,
    string: { ...struct.string, binary: 'Blob | Uint8Array' },
  }),
};
