//------------------------------------------------------------------------------
// Compilation.cpp
// Central manager for compilation processes
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/compilation/Compilation.h"

#include "slang/binding/SystemSubroutine.h"
#include "slang/compilation/Definition.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/symbols/ASTVisitor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace {

using namespace slang;

// This visitor is used to touch every node in the AST to ensure that all lazily
// evaluated members have been realized and we have recorded every diagnostic.
struct DiagnosticVisitor : public ASTVisitor<DiagnosticVisitor, false, false> {
    DiagnosticVisitor(Compilation& compilation, const size_t& numErrors, uint32_t errorLimit) :
        compilation(compilation), numErrors(numErrors), errorLimit(errorLimit) {}

    template<typename T>
    void handle(const T& symbol) {
        handleDefault(symbol);
    }

    template<typename T>
    bool handleDefault(const T& symbol) {
        if (numErrors > errorLimit)
            return false;

        if constexpr (std::is_base_of_v<Symbol, T>) {
            auto declaredType = symbol.getDeclaredType();
            if (declaredType) {
                declaredType->getType();
                declaredType->getInitializer();
            }

            if constexpr (std::is_same_v<ParameterSymbol, T> ||
                          std::is_same_v<EnumValueSymbol, T>) {
                symbol.getValue();
            }

            for (auto attr : compilation.getAttributes(symbol))
                attr->getValue();
        }

        if constexpr (is_detected_v<getBody_t, T>)
            symbol.getBody().visit(*this);

        visitDefault(symbol);
        return true;
    }

    void handle(const ExplicitImportSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.importedSymbol();
    }

    void handle(const WildcardImportSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getPackage();
    }

    void handle(const ContinuousAssignSymbol& symbol) {
        if (!handleDefault(symbol))
            return;
        symbol.getAssignment();
    }

    void handle(const DefinitionSymbol& symbol) {
        if (numErrors > errorLimit)
            return;

        auto guard = ScopeGuard([saved = inDef, this] { inDef = saved; });
        inDef = true;
        handleDefault(symbol);
    }

    void handleInstance(const InstanceSymbol& symbol) {
        if (numErrors > errorLimit)
            return;

        if (!inDef)
            instanceCount[&symbol.definition]++;
        handleDefault(symbol);
    }

    void handle(const ModuleInstanceSymbol& symbol) { handleInstance(symbol); }
    void handle(const ProgramInstanceSymbol& symbol) { handleInstance(symbol); }
    void handle(const InterfaceInstanceSymbol& symbol) { handleInstance(symbol); }

    void handle(const PortSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        symbol.getConnection();
        for (auto attr : symbol.getConnectionAttributes())
            attr->getValue();
    }

    void handle(const InterfacePortSymbol& symbol) {
        if (!handleDefault(symbol))
            return;

        for (auto attr : symbol.connectionAttributes)
            attr->getValue();
    }

    void handle(const GenerateBlockSymbol& symbol) {
        if (!symbol.isInstantiated)
            return;

        handleDefault(symbol);
    }

    Compilation& compilation;
    const size_t& numErrors;
    flat_hash_map<const Symbol*, size_t> instanceCount;
    uint32_t errorLimit;
    bool inDef = false;
};

const Symbol* getInstanceOrDef(const Symbol* symbol) {
    while (symbol && symbol->kind != SymbolKind::Definition &&
           !InstanceSymbol::isKind(symbol->kind)) {
        auto scope = symbol->getParentScope();
        symbol = scope ? &scope->asSymbol() : nullptr;
    }
    return symbol;
};

} // namespace

namespace slang::Builtins {

void registerArrayMethods(Compilation&);
void registerConversionFuncs(Compilation&);
void registerEnumMethods(Compilation&);
void registerMathFuncs(Compilation&);
void registerMiscSystemFuncs(Compilation&);
void registerNonConstFuncs(Compilation&);
void registerQueryFuncs(Compilation&);
void registerStringMethods(Compilation&);
void registerSystemTasks(Compilation&);

} // namespace slang::Builtins

namespace slang {

Compilation::Compilation(const Bag& options) :
    options(options.getOrDefault<CompilationOptions>()), tempDiag({}, {}) {

    // Construct all built-in types.
    bitType = emplace<ScalarType>(ScalarType::Bit);
    logicType = emplace<ScalarType>(ScalarType::Logic);
    regType = emplace<ScalarType>(ScalarType::Reg);
    signedBitType = emplace<ScalarType>(ScalarType::Bit, true);
    signedLogicType = emplace<ScalarType>(ScalarType::Logic, true);
    signedRegType = emplace<ScalarType>(ScalarType::Reg, true);
    shortIntType = emplace<PredefinedIntegerType>(PredefinedIntegerType::ShortInt);
    intType = emplace<PredefinedIntegerType>(PredefinedIntegerType::Int);
    longIntType = emplace<PredefinedIntegerType>(PredefinedIntegerType::LongInt);
    byteType = emplace<PredefinedIntegerType>(PredefinedIntegerType::Byte);
    integerType = emplace<PredefinedIntegerType>(PredefinedIntegerType::Integer);
    timeType = emplace<PredefinedIntegerType>(PredefinedIntegerType::Time);
    realType = emplace<FloatingType>(FloatingType::Real);
    realTimeType = emplace<FloatingType>(FloatingType::RealTime);
    shortRealType = emplace<FloatingType>(FloatingType::ShortReal);
    stringType = emplace<StringType>();
    chandleType = emplace<CHandleType>();
    voidType = emplace<VoidType>();
    nullType = emplace<NullType>();
    eventType = emplace<EventType>();
    errorType = emplace<ErrorType>();

    // Register built-in types for lookup by syntax kind.
    knownTypes[SyntaxKind::ShortIntType] = shortIntType;
    knownTypes[SyntaxKind::IntType] = intType;
    knownTypes[SyntaxKind::LongIntType] = longIntType;
    knownTypes[SyntaxKind::ByteType] = byteType;
    knownTypes[SyntaxKind::BitType] = bitType;
    knownTypes[SyntaxKind::LogicType] = logicType;
    knownTypes[SyntaxKind::RegType] = regType;
    knownTypes[SyntaxKind::IntegerType] = integerType;
    knownTypes[SyntaxKind::TimeType] = timeType;
    knownTypes[SyntaxKind::RealType] = realType;
    knownTypes[SyntaxKind::RealTimeType] = realTimeType;
    knownTypes[SyntaxKind::ShortRealType] = shortRealType;
    knownTypes[SyntaxKind::StringType] = stringType;
    knownTypes[SyntaxKind::CHandleType] = chandleType;
    knownTypes[SyntaxKind::VoidType] = voidType;
    knownTypes[SyntaxKind::EventType] = eventType;
    knownTypes[SyntaxKind::Unknown] = errorType;

#define MAKE_NETTYPE(type)                                               \
    knownNetTypes[TokenKind::type##Keyword] = std::make_unique<NetType>( \
        NetType::type, LexerFacts::getTokenKindText(TokenKind::type##Keyword), *logicType)

    MAKE_NETTYPE(Wire);
    MAKE_NETTYPE(WAnd);
    MAKE_NETTYPE(WOr);
    MAKE_NETTYPE(Tri);
    MAKE_NETTYPE(TriAnd);
    MAKE_NETTYPE(TriOr);
    MAKE_NETTYPE(Tri0);
    MAKE_NETTYPE(Tri1);
    MAKE_NETTYPE(TriReg);
    MAKE_NETTYPE(Supply0);
    MAKE_NETTYPE(Supply1);
    MAKE_NETTYPE(UWire);

    knownNetTypes[TokenKind::Unknown] =
        std::make_unique<NetType>(NetType::Unknown, "<error>", *logicType);
    wireNetType = knownNetTypes[TokenKind::WireKeyword].get();

#undef MAKE_NETTYPE

    // Scalar types are indexed by bit flags.
    auto registerScalar = [this](auto type) {
        scalarTypeTable[type->getIntegralFlags().bits() & 0x7] = type;
    };
    registerScalar(bitType);
    registerScalar(logicType);
    registerScalar(regType);
    registerScalar(signedBitType);
    registerScalar(signedLogicType);
    registerScalar(signedRegType);

    defaultTimeScale.base = { TimeUnit::Nanoseconds, TimeScaleMagnitude::One };
    defaultTimeScale.precision = { TimeUnit::Nanoseconds, TimeScaleMagnitude::One };

    root = std::make_unique<RootSymbol>(*this);

    // Register all system tasks, functions, and methods.
    Builtins::registerArrayMethods(*this);
    Builtins::registerConversionFuncs(*this);
    Builtins::registerEnumMethods(*this);
    Builtins::registerMathFuncs(*this);
    Builtins::registerMiscSystemFuncs(*this);
    Builtins::registerNonConstFuncs(*this);
    Builtins::registerQueryFuncs(*this);
    Builtins::registerStringMethods(*this);
    Builtins::registerSystemTasks(*this);
}

Compilation::~Compilation() = default;
Compilation::Compilation(Compilation&&) = default;

void Compilation::addSyntaxTree(std::shared_ptr<SyntaxTree> tree) {
    if (finalized)
        throw std::logic_error("The compilation has already been finalized");

    if (&tree->sourceManager() != sourceManager) {
        if (!sourceManager)
            sourceManager = &tree->sourceManager();
        else {
            throw std::logic_error(
                "All syntax trees added to the compilation must use the same source manager");
        }
    }

    const SyntaxNode& node = tree->root();
    const SyntaxNode* topNode = &node;
    while (topNode->parent)
        topNode = topNode->parent;

    auto unit = emplace<CompilationUnitSymbol>(*this);
    unit->setSyntax(*topNode);
    root->addMember(*unit);
    compilationUnits.push_back(unit);

    for (auto& [n, meta] : tree->getMetadataMap()) {
        auto decl = &n->as<ModuleDeclarationSyntax>();
        defaultNetTypeMap.emplace(decl, &getNetType(meta.defaultNetType));

        switch (meta.unconnectedDrive) {
            case TokenKind::Pull0Keyword:
                unconnectedDriveMap.emplace(decl, UnconnectedDrive::Pull0);
                break;
            case TokenKind::Pull1Keyword:
                unconnectedDriveMap.emplace(decl, UnconnectedDrive::Pull1);
                break;
            default:
                break;
        }

        if (meta.timeScale)
            timeScaleDirectiveMap.emplace(decl, *meta.timeScale);
    }

    for (auto& name : tree->getGlobalInstantiations())
        globalInstantiations.emplace(name);

    if (node.kind == SyntaxKind::CompilationUnit) {
        for (auto member : node.as<CompilationUnitSyntax>().members)
            unit->addMembers(*member);
    }
    else {
        unit->addMembers(node);
    }

    syntaxTrees.emplace_back(std::move(tree));
    cachedParseDiagnostics.reset();
}

span<const std::shared_ptr<SyntaxTree>> Compilation::getSyntaxTrees() const {
    return syntaxTrees;
}

span<const CompilationUnitSymbol* const> Compilation::getCompilationUnits() const {
    return compilationUnits;
}

const RootSymbol& Compilation::getRoot() {
    if (finalized)
        return *root;

    ASSERT(!finalizing);
    finalizing = true;
    auto guard = ScopeGuard([this] { finalizing = false; });

    // Find modules that have no instantiations. Iterate the definitions map
    // before instantiating any top level modules, since that can cause changes
    // to the definition map itself.
    SmallVectorSized<const DefinitionSymbol*, 8> topDefinitions;
    for (auto& [key, definition] : definitionMap) {
        // Ignore definitions that are not top level. Top level definitions are:
        // - Always modules
        // - Not nested
        // - Have no non-defaulted parameters
        // - Not instantiated anywhere
        if (std::get<1>(key) != root.get() ||
            definition->definitionKind != DefinitionKind::Module) {
            continue;
        }

        if (globalInstantiations.find(definition->name) != globalInstantiations.end())
            continue;

        bool allDefaulted = true;
        for (auto param : definition->parameters) {
            if (!param->hasDefault()) {
                allDefaulted = false;
                break;
            }
        }
        if (!allDefaulted)
            continue;

        topDefinitions.append(definition);
    }

    // Sort the list of definitions so that we get deterministic ordering of instances;
    // the order is otherwise dependent on iterating over a hash table.
    std::sort(topDefinitions.begin(), topDefinitions.end(),
              [](auto a, auto b) { return a->name < b->name; });

    SmallVectorSized<const ModuleInstanceSymbol*, 4> topList;
    for (auto def : topDefinitions) {
        auto& instance = ModuleInstanceSymbol::instantiate(*this, def->name, def->location, *def);
        root->addMember(instance);
        topList.append(&instance);
    }

    root->topInstances = topList.copy(*this);
    root->compilationUnits = compilationUnits;
    finalized = true;
    return *root;
}

const CompilationUnitSymbol* Compilation::getCompilationUnit(
    const CompilationUnitSyntax& syntax) const {

    for (auto unit : compilationUnits) {
        if (unit->getSyntax() == &syntax)
            return unit;
    }
    return nullptr;
}

const DefinitionSymbol* Compilation::getDefinition(string_view lookupName,
                                                   const Scope& scope) const {
    const Scope* searchScope = &scope;
    while (searchScope) {
        auto it = definitionMap.find(std::make_tuple(lookupName, searchScope));
        if (it != definitionMap.end())
            return it->second;

        auto& sym = searchScope->asSymbol();
        if (sym.kind == SymbolKind::Root)
            return nullptr;

        searchScope = sym.getLexicalScope();
    }

    return nullptr;
}

const DefinitionSymbol* Compilation::getDefinition(string_view lookupName) const {
    return getDefinition(lookupName, *root);
}

const Definition* Compilation::getDefinition2(string_view lookupName, const Scope& scope) const {
    const Scope* searchScope = &scope;
    while (searchScope) {
        auto it = definitionMap2.find(std::make_tuple(lookupName, searchScope));
        if (it != definitionMap2.end())
            return it->second.get();

        auto& sym = searchScope->asSymbol();
        if (sym.kind == SymbolKind::Root)
            return nullptr;

        searchScope = sym.getLexicalScope();
    }

    return nullptr;
}

const Definition* Compilation::getDefinition2(string_view lookupName) const {
    return getDefinition2(lookupName, *root);
}

void Compilation::addDefinition(const DefinitionSymbol& definition) {
    // Record that the given scope contains this definition. If the scope is a compilation unit, add
    // it to the root scope instead so that lookups from other compilation units will find it.
    const Scope* scope = definition.getParentScope();
    ASSERT(scope);

    if (scope->asSymbol().kind == SymbolKind::CompilationUnit)
        definitionMap.emplace(std::make_tuple(definition.name, root.get()), &definition);
    else
        definitionMap.emplace(std::make_tuple(definition.name, scope), &definition);
}

const Definition* Compilation::createDefinition(const Scope& scope, LookupLocation location,
                                                const ModuleDeclarationSyntax& syntax) {
    auto def =
        std::make_unique<Definition>(scope, location, syntax, getDefaultNetType(syntax),
                                     getUnconnectedDrive(syntax), getDirectiveTimeScale(syntax));
    auto result = def.get();

    // Record that the given scope contains this definition. If the scope is a compilation unit, add
    // it to the root scope instead so that lookups from other compilation units will find it.
    auto targetScope = scope.asSymbol().kind == SymbolKind::CompilationUnit ? root.get() : &scope;
    definitionMap2.emplace(std::tuple(def->name, targetScope), std::move(def));

    return result;
}

const PackageSymbol* Compilation::getPackage(string_view lookupName) const {
    auto it = packageMap.find(lookupName);
    if (it == packageMap.end())
        return nullptr;
    return it->second;
}

void Compilation::addPackage(const PackageSymbol& package) {
    packageMap.emplace(package.name, &package);
}

void Compilation::addSystemSubroutine(std::unique_ptr<SystemSubroutine> subroutine) {
    subroutineMap.emplace(subroutine->name, std::move(subroutine));
}

void Compilation::addSystemMethod(SymbolKind typeKind, std::unique_ptr<SystemSubroutine> method) {
    methodMap.emplace(std::make_tuple(string_view(method->name), typeKind), std::move(method));
}

const SystemSubroutine* Compilation::getSystemSubroutine(string_view name) const {
    auto it = subroutineMap.find(name);
    if (it == subroutineMap.end())
        return nullptr;
    return it->second.get();
}

const SystemSubroutine* Compilation::getSystemMethod(SymbolKind typeKind, string_view name) const {
    auto it = methodMap.find(std::make_tuple(name, typeKind));
    if (it == methodMap.end())
        return nullptr;
    return it->second.get();
}

void Compilation::setAttributes(const Symbol& symbol,
                                span<const AttributeSymbol* const> attributes) {
    attributeMap[&symbol] = attributes;
}

void Compilation::setAttributes(const Statement& stmt,
                                span<const AttributeSymbol* const> attributes) {
    attributeMap[&stmt] = attributes;
}

void Compilation::setAttributes(const Expression& expr,
                                span<const AttributeSymbol* const> attributes) {
    attributeMap[&expr] = attributes;
}

span<const AttributeSymbol* const> Compilation::getAttributes(const Symbol& symbol) const {
    return getAttributes(static_cast<const void*>(&symbol));
}

span<const AttributeSymbol* const> Compilation::getAttributes(const Statement& stmt) const {
    return getAttributes(static_cast<const void*>(&stmt));
}

span<const AttributeSymbol* const> Compilation::getAttributes(const Expression& expr) const {
    return getAttributes(static_cast<const void*>(&expr));
}

span<const AttributeSymbol* const> Compilation::getAttributes(const void* ptr) const {
    auto it = attributeMap.find(ptr);
    if (it == attributeMap.end())
        return {};

    return it->second;
}

const NameSyntax& Compilation::parseName(string_view name) {
    Diagnostics localDiags;
    SourceManager& sourceMan = SyntaxTree::getDefaultSourceManager();
    Preprocessor preprocessor(sourceMan, *this, localDiags);
    preprocessor.pushSource(sourceMan.assignText(name));

    Parser parser(preprocessor);
    auto& result = parser.parseName();

    if (!localDiags.empty()) {
        localDiags.sort(sourceMan);
        throw std::runtime_error(DiagnosticEngine::reportAll(sourceMan, localDiags));
    }

    return result;
}

CompilationUnitSymbol& Compilation::createScriptScope() {
    auto unit = emplace<CompilationUnitSymbol>(*this);
    root->addMember(*unit);
    return *unit;
}

const Diagnostics& Compilation::getParseDiagnostics() {
    if (cachedParseDiagnostics)
        return *cachedParseDiagnostics;

    cachedParseDiagnostics.emplace();
    for (const auto& tree : syntaxTrees)
        cachedParseDiagnostics->appendRange(tree->diagnostics());

    if (sourceManager)
        cachedParseDiagnostics->sort(*sourceManager);
    return *cachedParseDiagnostics;
}

const Diagnostics& Compilation::getSemanticDiagnostics() {
    if (cachedSemanticDiagnostics)
        return *cachedSemanticDiagnostics;

    // If we haven't already done so, touch every symbol, scope, statement,
    // and expression tree so that we can be sure we have all the diagnostics.
    DiagnosticVisitor visitor(*this, numErrors,
                              options.errorLimit == 0 ? UINT32_MAX : options.errorLimit);
    getRoot().visit(visitor);

    auto isInsideDef = [](const Symbol* symbol) {
        while (true) {
            if (symbol->kind == SymbolKind::Definition)
                return true;

            auto scope = symbol->getParentScope();
            if (!scope)
                return false;

            symbol = &scope->asSymbol();
        }
    };

    Diagnostics results;
    for (auto& pair : diagMap) {
        // Figure out which diagnostic from this group to issue.
        // If any of them are inside a definition (as opposed to one or more instances), issue
        // the one for the definition without embellishment. Otherwise, pick the first instance
        // and include a note about where the diagnostic occurred in the hierarchy.
        auto& [diagList, definitionIndex] = pair.second;
        if (definitionIndex < diagList.size()) {
            results.append(diagList[definitionIndex]);
            continue;
        }

        // Try to find a diagnostic in an instance that isn't at the top-level
        // (printing such a path seems silly).
        const Diagnostic* found = nullptr;
        const Symbol* inst = nullptr;
        size_t count = 0;

        for (auto& diag : diagList) {
            auto symbol = getInstanceOrDef(diag.symbol);
            if (!symbol || !symbol->getParentScope())
                continue;

            // Don't count the diagnostic if it's inside a definition instead of an instance.
            if (isInsideDef(symbol))
                continue;

            count++;
            auto& parent = symbol->getParentScope()->asSymbol();
            if (parent.kind != SymbolKind::Root && parent.kind != SymbolKind::CompilationUnit) {
                found = &diag;
                inst = symbol;
            }
        }

        // If the diagnostic is present in all instances, don't bother
        // providing specific instantiation info.
        if (found && visitor.instanceCount[&inst->as<InstanceSymbol>().definition] > count) {
            Diagnostic diag = *found;
            diag.symbol = getInstanceOrDef(inst);
            diag.coalesceCount = count;
            results.append(std::move(diag));
        }
        else {
            results.append(diagList.front());
        }
    }

    if (sourceManager)
        results.sort(*sourceManager);

    cachedSemanticDiagnostics.emplace(std::move(results));
    return *cachedSemanticDiagnostics;
}

const Diagnostics& Compilation::getAllDiagnostics() {
    if (cachedAllDiagnostics)
        return *cachedAllDiagnostics;

    cachedAllDiagnostics.emplace();
    cachedAllDiagnostics->appendRange(getParseDiagnostics());
    cachedAllDiagnostics->appendRange(getSemanticDiagnostics());

    if (sourceManager)
        cachedAllDiagnostics->sort(*sourceManager);
    return *cachedAllDiagnostics;
}

void Compilation::addDiagnostics(const Diagnostics& diagnostics) {
    for (auto& diag : diagnostics)
        addDiag(diag);
}

Diagnostic& Compilation::addDiag(Diagnostic diag) {
    auto isSuppressed = [](const Symbol* symbol) {
        while (symbol) {
            if (symbol->kind == SymbolKind::GenerateBlock &&
                !symbol->as<GenerateBlockSymbol>().isInstantiated)
                return true;

            auto scope = symbol->getParentScope();
            symbol = scope ? &scope->asSymbol() : nullptr;
        }
        return false;
    };

    // Filter out diagnostics that came from inside an uninstantiated generate block.
    ASSERT(diag.symbol);
    ASSERT(diag.location);
    if (isSuppressed(diag.symbol)) {
        tempDiag = std::move(diag);
        return tempDiag;
    }

    auto inst = getInstanceOrDef(diag.symbol);

    // Coalesce diagnostics that are at the same source location and have the same code.
    if (auto it = diagMap.find({ diag.code, diag.location }); it != diagMap.end()) {
        auto& [diagList, defIndex] = it->second;
        diagList.emplace_back(std::move(diag));
        if (inst && inst->kind == SymbolKind::Definition)
            defIndex = diagList.size() - 1;
        return diagList.back();
    }

    if (diag.isError())
        numErrors++;

    std::pair<std::vector<Diagnostic>, size_t> newEntry;
    newEntry.first.push_back(std::move(diag));
    if (inst && inst->kind == SymbolKind::Definition)
        newEntry.second = 0;
    else
        newEntry.second = SIZE_MAX;

    auto [it, inserted] =
        diagMap.emplace(std::make_tuple(diag.code, diag.location), std::move(newEntry));
    auto& [diagList, defIndex] = it->second;
    return diagList.back();
}

const NetType& Compilation::getDefaultNetType(const ModuleDeclarationSyntax& decl) const {
    auto it = defaultNetTypeMap.find(&decl);
    if (it == defaultNetTypeMap.end())
        return getNetType(TokenKind::Unknown);
    return *it->second;
}

UnconnectedDrive Compilation::getUnconnectedDrive(const ModuleDeclarationSyntax& decl) const {
    auto it = unconnectedDriveMap.find(&decl);
    if (it == unconnectedDriveMap.end())
        return UnconnectedDrive::None;
    return it->second;
}

optional<TimeScale> Compilation::getDirectiveTimeScale(const ModuleDeclarationSyntax& decl) const {
    auto it = timeScaleDirectiveMap.find(&decl);
    if (it == timeScaleDirectiveMap.end())
        return std::nullopt;
    return it->second;
}

const Type& Compilation::getType(SyntaxKind typeKind) const {
    auto it = knownTypes.find(typeKind);
    return it == knownTypes.end() ? *errorType : *it->second;
}

const Type& Compilation::getType(const DataTypeSyntax& node, LookupLocation location,
                                 const Scope& parent, bool forceSigned) {
    return Type::fromSyntax(*this, node, location, parent, forceSigned);
}

const Type& Compilation::getType(const Type& elementType,
                                 const SyntaxList<VariableDimensionSyntax>& dimensions,
                                 LookupLocation location, const Scope& scope) {
    return UnpackedArrayType::fromSyntax(*this, elementType, location, scope, dimensions);
}

const Type& Compilation::getType(bitwidth_t width, bitmask<IntegralFlags> flags) {
    ASSERT(width > 0);
    uint32_t key = width;
    key |= uint32_t(flags.bits()) << SVInt::BITWIDTH_BITS;
    auto it = vectorTypeCache.find(key);
    if (it != vectorTypeCache.end())
        return *it->second;

    auto type =
        emplace<PackedArrayType>(getScalarType(flags), ConstantRange{ int32_t(width - 1), 0 });
    vectorTypeCache.emplace_hint(it, key, type);
    return *type;
}

const Type& Compilation::getScalarType(bitmask<IntegralFlags> flags) {
    Type* ptr = scalarTypeTable[flags.bits() & 0x7];
    ASSERT(ptr);
    return *ptr;
}

const NetType& Compilation::getNetType(TokenKind kind) const {
    auto it = knownNetTypes.find(kind);
    return it == knownNetTypes.end() ? *knownNetTypes.find(TokenKind::Unknown)->second
                                     : *it->second;
}

const Type& Compilation::getUnsignedIntType() {
    return getType(32, IntegralFlags::Unsigned | IntegralFlags::TwoState);
}

Scope::DeferredMemberData& Compilation::getOrAddDeferredData(Scope::DeferredMemberIndex& index) {
    if (index == Scope::DeferredMemberIndex::Invalid)
        index = deferredData.emplace();
    return deferredData[index];
}

void Compilation::trackImport(Scope::ImportDataIndex& index, const WildcardImportSymbol& import) {
    if (index != Scope::ImportDataIndex::Invalid)
        importData[index].push_back(&import);
    else
        index = importData.add({ &import });
}

span<const WildcardImportSymbol*> Compilation::queryImports(Scope::ImportDataIndex index) {
    if (index == Scope::ImportDataIndex::Invalid)
        return {};
    return importData[index];
}

} // namespace slang
