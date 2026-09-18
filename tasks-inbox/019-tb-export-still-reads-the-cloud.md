# `tb_export.py` archives a broker the boards no longer publish to

Both boards were cut over to the self-hosted instance on 2026-09-17. The
archiver still points at `thingsboard.cloud` and authenticates with a Cloud API
key (`X-Authorization: ApiKey`), which the self-hosted instance does not use —
it wants a JWT from a username/password login, which `tb_import.py`'s
`tb_client.py` already speaks.

**So the nightly archive is now frozen in place**: it holds everything up to
the cutover and will never gain another row. Nothing reports that, which is
exactly the shape this repo keeps paying for — a tool that succeeds at reading
a source that stopped being the source.

**Done means:** a server/auth option so the same tool archives from either, and
a decision about what keeps happening to the Cloud archive. The 404 142 rows
are already imported into the self-hosted instance, so the Cloud copy is now a
second backup rather than the record.

Deliberately NOT urgent: the VPS takes its own nightly `pg_dump` at 03:17.
