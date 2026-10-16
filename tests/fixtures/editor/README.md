# Editor busconfig fixtures

Byte-for-byte copies of the OpenPLC Editor's generator goldens
(`src/backend/shared/ethercat/__tests__/fixtures/*.busconfig.golden.json` in openplc-editor).
`test_editor_contract.c` parses them, so a change on either side of the busconfig contract
fails here. When the editor regenerates its goldens, copy them over and run the tests.
