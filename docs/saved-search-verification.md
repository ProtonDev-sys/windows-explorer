# Native saved-search query verification

## Current evidence

Later focused runs verify the protective physical-scope predicates: v25 passes all sixteen native metadata groups (`artifacts/continuation-saved-search-v25.log`), and v26 passes all nine native writer groups (`artifacts/continuation-search-v26.log`). Four routes compare complete native FileID membership for shallow exclusions, equal-root include/exclude and excluded direct children of shallow includes. Five altered predicates are refused atomically. Writer fixtures also verify newly added files after saving, relative dates and canonical path spelling. These focused results extend the scope evidence without replacing the older complete-suite checkpoint below.

The 2026-10-05 v23 complete local suite passed all sixteen active targets, with
three correct disposable-CI-only skips, in 325.11 seconds. The executable SHA-256
is `ae94ed350901158f8a4a45ffb198eb2654e74b730d965cfa25f83d92866d15b3`;
the report is `artifacts/continuation-full-tests-v23.log`. This includes
native query/Title semantics, typed date boundaries, native confirmed file
replacement and six exact ACL profiles. Complete-suite, visual and hosted limits
remain separate in [verification.md](verification.md).

The earlier v21 focused native-refinement run passed all three groups, recorded in
`artifacts/continuation-refinement-v21.log`. Typed/imported Today-to-Last-month,
nonzero-to-Empty and Kind replacements retain exact live/native-save/import/re-save
FileIDs, supported metadata, view and scope rules. Tests also preserve original
saved-source bytes/identities, unrelated OR/NOT clauses and an absolute DateCreated
restriction; entangled category replacement preserves failed outputs. Every one
of the twenty-three installed Kind expressions retains its complete resolved
native fields, with actual Document, Folder and Saved-search membership checks.
The v23 authored/installed App suites pass all 223/231 checks in 46.93/49.33
seconds, including actual native SelectedItem, complete ItemsSource, checked
rows, exact refinement FileIDs, metadata/history and re-save. The four App/Ribbon
targets pass in 119.31 seconds (`artifacts/continuation-app-ribbon-v23.log`),
executable SHA-256 `ae94ed350901158f8a4a45ffb198eb2654e74b730d965cfa25f83d92866d15b3`.
Focused group/check counts belong to their
own test inventories.

The v22 focused saved-search run passed all fifteen native metadata groups
(`artifacts/continuation-saved-search-v22.log`). Its public Boolean fixture
checks the installed scalar `VT_BOOL` schema for `System.IsFolder`, independent
typed-factory results and public XML leaves with omitted `propertyType` for
`TRUE`, `FALSE` and `true`. Native open, imported live search and re-save retain
the exact owned folder/file identities. Original XML bytes and FileIDs, source
contents and outside-scope controls remain unchanged; malformed literals,
non-Boolean schemas, unknown properties and incompatible types preserve failed
caller outputs. This focused proof does not expand unsupported scope semantics.

The Title fixture preserves all five XML escapes and every exact 21-unit UTF-16
literal occurrence. Windows resolves the original query to three identical OR
leaves and the imported/restated query to nine. The test compares complete leaf
property, operation, VARTYPE, semantic type and literal value, normalizing only
Boolean associativity, duplicate identity and singleton compounds. It retains
all raw branches before normalization and rejects real native counterexamples
with changed value, property, operation, NOT or distinct AND/OR semantics. This
test correction changes no production writer or reader behavior; This PC still
requires its actual known-folder GUID and canonical native scope identity.

## Query semantics and exact results

Ordinary terms, quoted words, multiword queries and typed generic terms retain
the native parser's full conditions through save, native open, import, refinement
and re-save. Generic and named Blurb leaves resolve through their original
`IQuerySolution` context with public
[IConditionFactory::Resolve](https://learn.microsoft.com/en-us/windows/win32/api/structuredquery/nf-structuredquery-iconditionfactory-resolve).
`SQRO_DONT_SPLIT_WORDS` retains word groups;
[`SQRO_DONT_RESOLVE_DATETIME`](https://learn.microsoft.com/en-us/windows/win32/api/structuredquery/ne-structuredquery-structured_query_resolve_option)
keeps relative dates unresolved. Complete expansions, such as Generic.Integer
and Generic.String alternatives, are retained. Writer and reader share supported
scalar types and reject unsupported unresolved values before publication.

Resolved numeric/Boolean values are restated with public
[IQueryParser::RestateToString](https://learn.microsoft.com/en-us/windows/win32/api/structuredquery/nf-structuredquery-iqueryparser-restatetostring),
reparsed and checked for the identical canonical property, operation, scalar
type and value. A FileExtension Blurb equality previously gave no native results;
its resolved String condition now retains the exact owned identities. Native
`word eq` is `COP_WORD_EQUAL`, while `wordmatch` is `COP_WORD_STARTSWITH`.
Explicit filename prefixes retain exact live/native/import/re-save identities
for single/multiple words, Unicode, AND/OR/NOT, shallow scopes and extensions.
Unresolved date tokens and complete resolved fingerprints remain required.

The [public saved-search format](https://learn.microsoft.com/en-us/windows/win32/search/-search-savedsearchfileformat)
includes Boolean leaves without `propertyType`. The reader infers only an
installed scalar Boolean property description, constructs the typed leaf with
[IConditionFactory2::CreateBooleanLeaf](https://learn.microsoft.com/en-us/windows/win32/api/structuredquery/nf-structuredquery-iconditionfactory2-createbooleanleaf)
and accepts only exact `TRUE`/`FALSE` literals, ignoring letter case. Native
restatement adds the Boolean semantic name omitted by the typed factory. The
reader attaches only that observed native Boolean name through the public
factory and verifies identical resolved property, operation, VARTYPE, semantic
type and value before publishing editable metadata. It never infers other
omitted scalar types.

Owned fixtures compare strict cardinality, volume/128-bit FileIDs, duplicate
rejection and outside-scope exclusion across four routes: original live search,
native `.search-ms` open, imported live search and a second saved search. Cases
cover 23 generic queries, prefixes, numeric/fractional terms, comparisons/ranges,
OR/NOT, mixed property/generic conditions, relative dates and equal basenames.
A saved query discovers a new matching file, excludes an outside file and stops
returning an identity renamed to a nonmatching name. The installed unindexed
provider treats multiword prefixes as an unordered set and can match a quoted
multiword query in separated or reversed filenames. Tests preserve that actual
native behavior without adding an application phrase matcher.

Typed Date modified `start..end` ranges use public
[MakeLeaf](https://learn.microsoft.com/en-us/windows/win32/api/structuredquery/nf-structuredquery-iconditionfactory-makeleaf)
to serialize the equivalent inclusive scalar bounds inside an `andCondition`.
Relative Today/Tomorrow values remain unresolved. Seventeen timestamped files
verify leap day, year boundaries, strict comparisons, inclusive/negated ranges,
size refinement, UTC seconds, midnight and the last 100 nanoseconds of a day.
All four routes require expected identities and unchanged contents/timestamps.
Current-zone checks observe this PC's 23-hour spring and 25-hour autumn days
without changing its time zone, following native
[AQS date semantics](https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax).
The reader permits native NOT distribution over a range. Canonical UTC text is
accepted only after public resolution proves the same one-second interval and
reparsing preserves all native fields; relative dates are never frozen or native
date tokens decoded. Invalid February dates that become generic fallback
conditions are rejected with caller/output preservation.

## Scope, metadata and provider limits

The new refinement model inspects the complete native condition outside Ribbon
property callbacks. It splits only AND factors, replaces whole pure-category
subtrees and preserves unrelated OR/NOT conditions. Entangled categories refuse
replacement before query, live-search intent or history mutation. Native
restatement/reparse validation compares every resolved leaf field and unresolved
relative-date token; actual preset selection is cached from the full category
predicate. The exact eight installed Date rows remain unchanged. The native
helper's complete-field and exact-FileID tests pass in v21; v23 passes actual App
selection, metadata/history and atomic entangled-category rejection in both
layouts. Lazy gallery publication restores native selection through deferred
value invalidation after its complete rows are registered. Tests expand and
collapse only the real visible gallery parent on the private desktop before
native state readback; no filter leaf or forced property write supplies the
selected result.

Kind queries come from actual installed enum values through native MakeLeaf,
restatement and complete reparse/Resolve validation. Quoting raw enum values had
returned no Document results. An explicit String semantic type now matches the
resolved native enum fields. When native restatement loses a Kind property, as
observed for SearchFolder, a bounded OS identifier supplies the documented
[canonical enum marker](https://learn.microsoft.com/en-us/windows/win32/search/-search-3x-advancedquerysyntax).
Acceptance still requires every resolved field to equal the installed enum value;
the twenty-three-row test permits no truncation or weakened comparison.

Multiple locations and native Library unions survive live search, import,
refinement and re-save. Public `IShellLibrary` locations expand without writing
user Libraries or indexing settings. The owned two-location fixture covers equal
basenames, nested matches, shallow locations and newly added results. Scope rules
retain every include/exclude location and recursion flag, up to 256 locations.
Native ItemFolderPath/ItemPath conditions constrain mixed recursion and recursive
exclusions; fixtures cover overlapping roots and multiple exclusions. Only tests
retry transient native snapshots, for at most five seconds. Production performs
no recursive application scan.

The v23 unindexed provider ignored shallow and equal-root exclusions, and an
excluded direct child of a shallow include retained its folder object. Later
source adds an exact native path predicate around the untouched original query
for these physical shapes while retaining their public scope XML and flags.
Editable import requires exactly one complete matching outer guard and removes
only that guard; unguarded or altered external queries remain native-view-only.
The new native membership/import tests await a later runtime checkpoint. Virtual
exclusions, mixed virtual roots/shallow physical roots and other unsupported
external shapes remain native-view-only. No indexed-provider behavior is inferred.

Native kind unions retain typed property values and validated restated text.
Five fixtures cover document/picture, normalized picture/folder, folder-only,
repeated document and item/picture. The installed loader uses a sole item entry
as its all-item sentinel, item/picture as picture-only and repeated item entries
as an empty union; repeated all-item sentinels remain native-view-only. Literal
percent scope names retain native identity. Exact known-folder roots use public
GUIDs and canonical Shell identity. This PC, Network and Control Panel receive
metadata/PIDL checks without user-wide enumeration; arbitrary ProgIDs are
rejected before native class conversion.

Simple-text `author`, `kind`, `description` and `tags` retain exact values through
import, refinement, history and re-save, bounded to 32,768 UTF-16 units each.
Unknown/nested shapes remain native-view-only. Unicode, whitespace, empty
properties, CDATA/text mixtures and escaped boundary values are covered. Since
MSXML normalizes adjacent CR/LF references, a bounded XmlLite pass reads exact
text from the same validated bytes. Both parsers prohibit DTDs and external
resolution. Complete generated XML must satisfy reader byte/depth/node limits
before opening a destination: a 1,800-term query fits with one scope, while 256
scopes exceed the node budget and preserve existing bytes/FileID. Failed reads
preserve every caller field, including scope arrays and rules.

Long Unicode scopes verify shallow/deep native identities and typed executable
FileID beyond `MAX_PATH`; extended prefixes are used for I/O while ordinary Shell
names resolve the same identities. On installed Windows 10, a saved file whose
own filename exceeds `MAX_PATH` still resolves as a Shell item, but native loader
binding/browsing returns `ERROR_INSUFFICIENT_BUFFER`. Bytes/FileID remain intact.
The App handles only that recognized error through the original public native
condition/scope factory; this does not extend stock loader support or suppress
other errors. Same-current-query reuse retains scope, metadata, presentation and
native history rather than browsing another equivalent wrapper.

## Publication, permissions and presentation

The native Save dialog retains
[`FOS_OVERWRITEPROMPT`](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions).
Only a successful dialog authorizes `SearchSaveMode::UserConfirmed`; the default
writer uses `CREATE_NEW`. Validation and full UTF-8 serialization precede output.
Replacement uses a flushed, create-new same-directory temporary and public
[SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)
with `FILE_RENAME_INFO`, retaining exact DACL, creation time, ordinary attributes
and native compression. POSIX rename lets existing readers retain the old file;
DELETE-access preflight respects sharing denial. Cleanup uses the owned handle.
Rejected saves preserve original bytes, identity, DACL and timestamps with no
staging residue. Hidden/compressed/read-only and locked targets are covered;
EFS-encrypted/reparse targets are refused, and arbitrary alternate streams are
not claimed preserved.

Asynchronous
[Shell change notification](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify)
follows closed handles. Native providers can cache prior conditions until their
STA processes notifications. Tests check published XML immediately, then pump
that STA and require exact new identities within five seconds. Production waits
for no global recipient and performs no result scan.

Query and companion replacement read the exact retained-handle descriptor with
[GetKernelObjectSecurity](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-getkernelobjectsecurity)
and assign it through
[CreateFileW security attributes](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
No `SetKernelObjectSecurity` is used on files. Conditional inheritance conversion
for already auto-inherited sources happens while the temporary is empty. Exact
DACL presence, inheritance/protection controls, ACE order, flags, masks and
trustee bytes must match before writing data and again before publication.
Changed-parent or concurrent ACL mismatches refuse replacement with the original
intact. No privilege is enabled or original permission modified. Six owned
profiles cover legacy inherited/explicit-unprotected, protected, modern
inherited/protected and explicit data-write-deny descriptors, with independent
SDDL/control comparison, native identities and locked-file preservation.

The [public saved-search format](https://learn.microsoft.com/en-us/windows/win32/search/-search-savedsearchfileformat)
supports Details/Icons/Tiles, icon sizes 16–256, canonical columns, grouping and
sort direction. The bounded host model allows four sort keys. Unknown fields,
duplicates and unsupported child elements make external files native-view-only;
missing fields keep defaults. Installed `Windows.Storage.Search.dll` accepts the
three documented XML modes and rejects List/SmallIcons/Content tokens. Fresh
native browsers opened even supported modes in Content view, so XML alone is
not proof of restoration. The App applies and reads actual presentation through
`IFolderView2`/`IColumnManager`; the probed factory folders expose neither
`IPersistFile` nor `IFolderViewSettings`.

All eight App layouts use a versioned LocalAppData `saved-search-views` companion
where public XML cannot represent exact mode/icon size. This is App metadata,
without a claim that stock Explorer reads it. Matching uses canonical path,
volume/128-bit FileID, size and modification/change times; copied, replaced,
edited or renamed files cannot inherit stale layouts. Companions alter only
mode/icon size after public column/group/sort import and publish through flushed
create-new temporaries and atomic rename. Query and companion are separate
transactions: a companion failure reports a saved query whose view was not
saved. Foreign/malformed/locked records remain untouched; older unmatched files
are harmless and not silently deleted. Clearing the directory while the App is
closed resets only these layouts. Headless sessions use exclusively owned
directories and never resolve the user's companion directory.

The three-group private presentation fixture covers sixteen cases across eight
layouts, descending/no grouping, four columns and two sort keys. Native readback,
result FileIDs, contents, attributes and modification/change times must agree.
Every browser uses `EBO_NOPERSISTVIEWSTATE`; shared native Bags restoration is a
separate disposable-CI target. All fixtures preserve the input desktop, clipboard,
user settings, index and Recycle Bin and enumerate only owned scopes.

## Retained failures

| Snapshot / evidence | Failure and subsequent evidence |
| --- | --- |
| Earlier [CI 37235340344](https://github.com/ProtonDev-sys/windows-explorer/actions/runs/37235340344) | Confirmed-save DACL equality failed despite earlier default-permission success. Owned legacy probes reproduced added `SE_DACL_AUTO_INHERITED` (`0x8004` to `0x8404`) and reinterpreted legacy ACE flags/protection. The equality assertion remained; exact staging-before-data permission checks replaced blind inheritance-aware cloning. |
| 2026-10-05 `artifacts/continuation-full-tests-v11.log` | The Title test expected a root value attribute, but the native writer emitted an OR condition. |
| 2026-10-05 `artifacts/continuation-full-tests-v12.log` and `artifacts/title-diagnostics-v13-final.log` | Exact syntactic comparison rejected three versus nine identical native Title leaves; even `MakeAndOr(fSimplify=TRUE)` retained duplicates. The subsequent `artifacts/title-semantic-v13.log` passes complete leaf/Boolean meaning and real native counterexamples without changing production serialization. |

Microsoft's [automatic inheritance contract](https://learn.microsoft.com/en-us/windows/win32/secauthz/automatic-propagation-of-inheritable-aces)
explains why inheritance conversion cannot blindly clone legacy permissions.
Failure diagnostics expose only controls and ACE types/flags/masks/sizes, never
paths, SDDL or trustees. Obsolete `SetFileSecurity` is confined to constructing
owned legacy test descriptors. Historical focused timing does not establish a
passing newer full suite or hosted rerun.
