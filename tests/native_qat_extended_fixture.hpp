// Included after the independent native state, envelope and event helpers.
// The only >20 setup is the real source-owned UIA context gesture above.
// No synthetic IUICollection or token-list reconstruction is used.
void verifyExtendedNativeQuickAccess(explorer::NativeRibbon& ribbon, HWND owner,
    explorer::RibbonLayout layout, unsigned& executions, ULONGLONG deadline) {
    using namespace explorer;
    Directory files;
    const std::wstring name=layout==RibbonLayout::Authored?L"authored":L"installed";
    const auto settings=files.path/(name+L"-envelope.bin"),invalid=files.path/(name+L"-invalid.bin");
    const auto original=readNative(ribbon,owner,deadline,"extended-original-gesture21");
    require(original.rows.size()==21,"Extended preservation requires the genuine21-row native gesture result");
    auto captured=snapshot(ribbon,original);
    require(captured.items().size()==21,"Public native snapshot truncated gesture21");
    std::vector<UINT> projected;
    exact(ribbon.quickAccessCommands(projected),"Complete legacy projection of21 declared native commands");
    require(projected.size()==21,"Legacy projection truncated complete native21");
    require(quickAccessCommand(Undo)==nullptr,"Undo must remain outside the lossy token chooser in this fixture");
    require(std::find(projected.begin(),projected.end(),Undo)!=projected.end(),
        "Actual valid native Undo outside the token chooser was silently discarded");

    // A targeted edit retains all21 original native objects and their metadata.
    edit(ribbon,captured,RibbonQuickAccessEditKind::Move,0,20,0);
    auto expected=original;
    std::rotate(expected.rows.begin(),expected.rows.end()-1,expected.rows.end());
    auto moved=readNative(ribbon,owner,deadline,"extended-whole-row-move");
    exactState(expected,moved);
    require(ribbon.editQuickAccess(captured,{RibbonQuickAccessEditKind::Remove,0,0,0})==
        HRESULT_FROM_WIN32(ERROR_RETRY),"Stale native21 snapshot edited a changed order");
    exactState(moved,readNative(ribbon,owner,deadline,"extended-stale-order-preserved"));
    const auto savedOrder=nativeIds(moved);
    const bool savedMinimized=nativeMinimized(moved);
    exact(ribbon.saveSettings(settings),"Save complete genuine native21 with schema2");
    exactState(moved,readNative(ribbon,owner,deadline,"extended-save-preserved"));
    const auto bytes=readOwnedBytes(files,settings);
    require(envelopeUInt(bytes,8)==2 && envelopeUInt(bytes,12)==static_cast<UINT>(layout) &&
        envelopeUInt(bytes,16)==21,"Extended native state requires explicit schema2/count21/layout");
    const auto length=envelopeUInt(bytes,20);
    require(length && length<=65536 && bytes.size()==56+21*4+length,"Extended envelope has exact bounded opaque payload");
    const auto checksum=envelopeDigest(bytes);
    require(std::equal(checksum.begin(),checksum.end(),bytes.begin()+24),"Independent schema2 checksum mismatch");
    for(UINT index=0;index<21;++index)
        require(envelopeUInt(bytes,56+index*4)==savedOrder[index],"Extended native ID manifest lost exact order");
    const std::vector<BYTE> payload(bytes.begin()+56+21*4,bytes.end());

    // Independently save the current real IUIRibbon and byte-compare its opaque
    // output with the owned wrapper. Do not parse/reconstruct native bytes.
    ComPtr<IUIRibbon> nativeView;
    exact(ribbon.nativeFramework()->GetView(0,IID_PPV_ARGS(&nativeView)),"Retain actual extended native view");
    ComPtr<IStream> direct;
    exact(CreateStreamOnHGlobal(nullptr,TRUE,&direct),"Create independent native settings stream");
    exact(nativeView->SaveSettingsToStream(direct.Get()),"Independent actual native21 SaveSettingsToStream");
    STATSTG stat{};exact(direct->Stat(&stat,STATFLAG_NONAME),"Read independent opaque stream length");
    require(stat.cbSize.QuadPart==payload.size(),"Owned schema2 changed native payload length");
    LARGE_INTEGER zero{};exact(direct->Seek(zero,STREAM_SEEK_SET,nullptr),"Rewind actual independent native stream");
    std::vector<BYTE> directBytes(payload.size());ULONG read=0;
    exact(direct->Read(directBytes.data(),static_cast<ULONG>(directBytes.size()),&read),"Read full independent native stream");
    require(read==directBytes.size() && directBytes==payload,"Owned schema2 changed actual opaque native bytes");
    exactState(moved,readNative(ribbon,owner,deadline,"extended-direct-save-preserved"));

    // Structural errors are rejected before a native Load, leaving original
    // collection, all21 identities, metadata, dock and minimized state exact.
    const auto rejected=[&](std::vector<BYTE> bad,const char* phase) {
        writeOwnedBytes(files,invalid,bad);
        require(ribbon.loadSettings(invalid)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "Malformed extended envelope did not reject before native mutation");
        exactState(moved,readNative(ribbon,owner,deadline,phase));
        require(nativeMinimized(moved)==savedMinimized,"Malformed extended envelope changed minimized state");
    };
    auto bad=bytes;envelopeUInt(bad,8,1);rehashEnvelope(bad);rejected(bad,"schema1-still-rejects21");
    bad=bytes;envelopeUInt(bad,8,3);rehashEnvelope(bad);rejected(bad,"extended-unknown-version");
    bad=bytes;envelopeUInt(bad,16,513);rehashEnvelope(bad);rejected(bad,"extended-resource-bound");
    bad=bytes;envelopeUInt(bad,12,layout==RibbonLayout::Authored?1:0);rehashEnvelope(bad);rejected(bad,"extended-layout-mismatch");
    bad=bytes;envelopeUInt(bad,56,0);rehashEnvelope(bad);rejected(bad,"extended-zero-id");
    bad=bytes;envelopeUInt(bad,60,envelopeUInt(bad,56));rehashEnvelope(bad);rejected(bad,"extended-duplicate-id");
    bad=bytes;bad.back()^=1;rejected(bad,"extended-payload-checksum");
    bad=bytes;bad.pop_back();rejected(bad,"extended-truncated");
    bad=bytes;bad.push_back(0);rejected(bad,"extended-trailing-byte");
    // This correctly checksummed manifest reaches native Load. Its different
    // complete ID set must roll back to all21 original retained whole rows.
    bad=bytes;envelopeUInt(bad,56,0xfffffffeu);rehashEnvelope(bad);
    writeOwnedBytes(files,invalid,bad);
    require(ribbon.loadSettings(invalid)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
        "Extended native ID set mismatch was accepted");
    exactState(moved,readNative(ribbon,owner,deadline,"extended-exact-rollback21"));
    require(nativeMinimized(moved)==savedMinimized,"Extended mismatch rollback lost minimized state");

    const std::array<UINT,2> disturbance{Properties,NewFolder};
    exact(ribbon.setQuickAccessCommands(disturbance),"Disturb genuine21 only after its complete save");
    exact(ribbon.setQuickAccessBelow(moved.dock!=UI_CONTROLDOCK_BOTTOM),"Disturb extended saved dock");
    exact(ribbon.setMinimized(!savedMinimized),"Disturb extended saved minimized state");
    direct.Attach(SHCreateMemStream(payload.data(),static_cast<UINT>(payload.size())));
    require(direct!=nullptr,"Create unchanged independent native load stream");
    exact(nativeView->LoadSettingsFromStream(direct.Get()),"Independent actual native21 LoadSettingsFromStream");
    materialize(ribbon,owner,deadline);
    const auto nativeReference=readNative(ribbon,owner,deadline,"extended-independent-native-roundtrip");
    require(nativeReference.rows.size()==21,"Native opaque roundtrip itself did not retain21 rows");
    auto referenceSet=nativeIds(nativeReference),savedSet=savedOrder;
    std::sort(referenceSet.begin(),referenceSet.end());std::sort(savedSet.begin(),savedSet.end());
    require(referenceSet==savedSet,"Native opaque roundtrip itself lost an original ID");
    require(nativeReference.dock==moved.dock && nativeMinimized(nativeReference)==savedMinimized,
        "Independent native roundtrip lost dock/minimized state");

    exact(ribbon.setQuickAccessCommands(disturbance),"Disturb independent roundtrip before owned restore");
    exact(ribbon.setQuickAccessBelow(moved.dock!=UI_CONTROLDOCK_BOTTOM),"Disturb owned restore dock");
    exact(ribbon.setMinimized(!savedMinimized),"Disturb owned restore minimized state");
    exact(ribbon.loadSettings(settings),"Restore complete native21 envelope and exact original order");
    materialize(ribbon,owner,deadline);
    auto restored=readNative(ribbon,owner,deadline,"extended-owned-restore21");
    require(nativeIds(restored)==savedOrder && restored.dock==moved.dock &&
        nativeMinimized(restored)==savedMinimized,"Owned extended restore lost order/count/dock/state");
    // A native Load is allowed to create new COM rows. Compare complete
    // independently observed public row state by actual native ID, with only
    // the instance identity excluded from this cross-Load comparison.
    for(const auto& row:restored.rows) {
        const auto found=std::find_if(nativeReference.rows.begin(),nativeReference.rows.end(),
            [&](const auto& reference){return reference.command==row.command;});
        require(found!=nativeReference.rows.end(),"Owned restored row absent from native reference");
        auto reference=*found;reference.identity=row.identity;
        require(sameRow(reference,row),"Owned reorder changed independent native loaded row metadata/state");
    }
    auto restoredSnapshot=snapshot(ribbon,restored);
    edit(ribbon,restoredSnapshot,RibbonQuickAccessEditKind::Remove,0,20,0);
    auto removedExpected=restored;removedExpected.rows.pop_back();
    exactState(removedExpected,readNative(ribbon,owner,deadline,"extended-targeted-remove21-to20"));
    exact(ribbon.loadSettings(settings),"Restore full native21 after targeted removal");
    auto retirementState=readNative(ribbon,owner,deadline,"extended-retirement-source");
    auto retired=snapshot(ribbon,retirementState);
    ComPtr<IUnknown> retiredFramework;
    exact(ribbon.nativeFramework()->QueryInterface(IID_PPV_ARGS(&retiredFramework)),
        "Retain canonical identity of original21-row framework");

    // Actual native IDs outside the legacy chooser are covered by Undo above.
    // Authored layout has no demonstrated unmapped native command; do not
    // pretend a fabricated property set is a genuine provider ID.
    UINT actualUnmapped=0;
    for(const auto& item:retired.items())if(item.commandRead==S_OK&&!item.command)++actualUnmapped;
    std::cout<<"QAT extended actual unmapped native IDs="<<actualUnmapped
        <<" chooserUnknownUndoCovered=1 fabricatedNativeIds=0"<<std::endl;
    ribbon.reset();require(!ribbon.valid(),"Retire exact actual native21 framework");
    RibbonCallbacks callbacks;
    callbacks.execute=[&](UINT){++executions;return E_ACCESSDENIED;};
    callbacks.executeItem=[&](UINT,UINT){++executions;return E_ACCESSDENIED;};
    callbacks.query=[](UINT command){RibbonCommandState state;state.enabled=command!=Copy;return state;};
    exact(ribbon.initialize(owner,GetModuleHandleW(nullptr),std::move(callbacks),layout),
        "Reinitialize fresh actual framework after retiring21-row source");
    ComPtr<IUnknown> freshFramework;
    exact(ribbon.nativeFramework()->QueryInterface(IID_PPV_ARGS(&freshFramework)),
        "Read canonical identity of fresh framework");
    require(freshFramework.Get()!=retiredFramework.Get(),"Native21 retirement reused the retained framework identity");
    auto fresh=readNative(ribbon,owner,deadline,"extended-fresh-generation");
    require(ribbon.editQuickAccess(retired,{RibbonQuickAccessEditKind::Remove,0,20,0})==
        HRESULT_FROM_WIN32(ERROR_RETRY),"Retired21-row snapshot edited fresh framework");
    exactState(fresh,readNative(ribbon,owner,deadline,"extended-retired-generation-preserved"));
    require(executions==0,"Extended native preservation executed an application/Shell command");
    isolation(owner,deadline);
    std::cout<<"PASS native gesture21 preservation schema2=1 completeSnapshot=1 mappedProjection21=1"
        <<" originalWholeRowsMove=1 targetedRemove=1 schema1StillRejects21=1 opaqueBytesExact="<<payload.size()
        <<" independentNativeRoundtrip21=1 exactOrder=1 exactNativeLoadedMetadata=1 exactWholeRollback21=1"
        <<" retiredGeneration=1 fakeCollection=0"<<std::endl;
}
