# Friend-list server investigation

This note records the static and schema evidence used by `friend_list_fix`. It
contains no player rows, credentials, or server configuration.

## Owning host and ABI

The persistent list request is owned by the verified
`WorldServer-548A86E3-0010C000` image (SHA-256
`82d3c4cf09679a431b9bfa639795b148f932642d8774ff66021785115dc53d46`).
`CRequest::ReqFriend_List` calls `COleGameDB::SP_GetFriendList` at RVA
`0x2EE60` with five stack arguments:

1. the authenticated native OLE DB connection,
2. a packed five-`INT` parameter record,
3. the output row buffer,
4. row capacity, and
5. a separate result-row count pointer.

The parameter record is account ID, page index, page size, `@retVal OUTPUT`,
and `@TotalCnt OUTPUT`. The request initializes page size to eight,
`@retVal` to `-1`, and `@TotalCnt` to zero. It does not consume `@retVal`
after the call. The executor's separate row count controls the number of rows
encoded in opcode `0x4B3`; `@TotalCnt` is copied to the response header.

The wrapper returns zero on success. Its native row-execution failure returns
`1000`; any nonzero value is handled identically by the request and produces
native friend-list error `0x0813`. A repair initialization failure may
therefore return a nonzero value without publishing an empty successful list.

## Result contract and nullable-row failure

The native wrapper registers exactly 17 columns and uses a packed row size of
`0x5D` bytes:

| Column | SQL/native type | Packed offset |
| --- | --- | ---: |
| friend ID | `int` | `0x00` |
| pilot name | `varchar`, native `DBTYPE_STR[17]` | `0x04` |
| relation | `int` | `0x15` |
| gender | native `DBTYPE_STR[2]` | `0x19` |
| cutoff | native `DBTYPE_STR[2]` | `0x1B` |
| level | `int` | `0x1D` |
| level name | native `DBTYPE_STR[11]` | `0x21` |
| clan mark 1 | `int` | `0x2C` |
| clan mark 2 | `int` | `0x30` |
| channel | `int` | `0x34` |
| location | `tinyint` | `0x38` |
| room | `int` | `0x39` |
| team | `int` | `0x3D` |
| agent | `int` | `0x41` |
| registration date | `DBTYPE_DBTIMESTAMP` | `0x45` |
| emblem | `int` | `0x55` |
| level step | `int` | `0x59` |

The shared native row executor constructs each `DBBINDING` with
`dwPart=DBPART_VALUE` only. It does not bind `DBPART_STATUS` or a status
buffer. Its row loop stops as soon as `IRowset::GetData` returns any nonzero
HRESULT, including a successful status-warning result, while the wrapper can
still report overall success. This explains why the server log need not show
an `SP_GetFriendList execute` failure when a list is truncated or unusable.

The stock procedure left-joins game information, online presence, clan, and
emblem tables. Their projected fields can consequently be `NULL`, especially
for offline friends. The owned clone keeps the original five-parameter and
17-column shape but returns a non-null value of the same native type for every
optional field. The three invariant fields (friend ID, pilot name, relation)
remain direct values. Pilot name is selected from the canonical profile row,
with the stored friendship name as a fallback, and converted back to
`varchar(20)` for the native ANSI binding.

The stock procedure's `@retVal = @@ROWCOUNT` is retained for compatibility,
but the request does not use it as the row count. Tests should assert the
separate returned row count and `@TotalCnt`, not interpret `@retVal` as a
reliable packet field.

## Whisper behavior

`CRequest::ReqWhisperPilot` resolves the target through
`GOnlineGame.dbo.sp_GetUserAgentPort`. The procedure returns a nonzero result
when the pilot name does not exist and returns `ISNULL(AgentPort,0)` for an
existing pilot. WorldServer maps a missing pilot to error `0x1406`, maps
`AgentPort=0` to offline error `0x1407`, and forwards the message only when the
port is nonzero.

There is no server-side offline whisper queue on this path. The client repair
can retain an offline friend's native conversation entry, while an attempted
send to that offline friend must continue to fail visibly rather than appear
queued or delivered.

## Verification boundary

The six `friends.*` symbols were checked against the exact WorldServer bytes
with `tools/verify_symbols.py`; the complete WorldServer profile reported 23
symbols and zero rejections. Database inspection was read-only. Synthetic SQL
fixtures, rather than player data, cover all-offline pagination, missing
optional rows, canonical-name refresh, fallback names, online/logout
transitions, empty lists, and clamped pages.
