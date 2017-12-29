//------------------------------------------------------------------------------
// Scope.cpp
// Base class for symbols that represent lexical scopes.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "Scope.h"

#include "compilation/Compilation.h"
#include "symbols/Symbol.h"

namespace slang {

const LookupRefPoint LookupRefPoint::max{ nullptr, UINT_MAX };
const LookupRefPoint LookupRefPoint::min{ nullptr, 0 };

LookupRefPoint LookupRefPoint::before(const Symbol& symbol) {
    return LookupRefPoint(symbol.getScope(), (uint32_t)symbol.getIndex());
}

LookupRefPoint LookupRefPoint::after(const Symbol& symbol) {
    return LookupRefPoint(symbol.getScope(), (uint32_t)symbol.getIndex() + 1);
}

LookupRefPoint LookupRefPoint::startOfScope(const Scope& scope) {
    return LookupRefPoint(&scope, 0);
}

LookupRefPoint LookupRefPoint::endOfScope(const Scope& scope) {
    return LookupRefPoint(&scope, UINT32_MAX);
}

bool LookupRefPoint::operator<(const LookupRefPoint& other) const {
    return index < other.index;
}

void LookupResult::clear() {
    nameKind = LookupNameKind::Local;
    referencePoint = LookupRefPoint::max;
    resultKind = NotFound;
    resultWasImported = false;
    symbol = nullptr;
    imports.clear();
}

void LookupResult::setSymbol(const Symbol& found, bool wasImported) {
    symbol = &found;
    resultWasImported = wasImported;
    resultKind = Found;
}

void LookupResult::addPotentialImport(const Symbol& import) {
    if (!imports.empty())
        resultKind = AmbiguousImport;
    imports.append(&import);
}

Scope::Scope(Compilation& compilation_, const Symbol* thisSym_) :
    compilation(compilation_), thisSym(thisSym_),
    nameMap(compilation.allocSymbolMap())
{
}

Scope::iterator& Scope::iterator::operator++() {
    current = current->nextInScope;
    return *this;
}

const Scope* Scope::getParent() const {
    return thisSym->getScope();
}

void Scope::addMember(const Symbol& symbol) {
    // For any symbols that expose a type, keep track of it in our
    // deferred data so that we can include enum values in our member list.
    const LazyType* lazyType = nullptr;
    switch (symbol.kind) {
        case SymbolKind::Variable:
        case SymbolKind::FormalArgument:
            lazyType = &symbol.as<VariableSymbol>().type;
            break;
        case SymbolKind::Subroutine:
            lazyType = &symbol.as<SubroutineSymbol>().returnType;
            break;
        case SymbolKind::Parameter:
            lazyType = &symbol.as<ParameterSymbol>().getLazyType();
            break;
        default:
            break;
    }

    if (lazyType) {
        auto syntax = lazyType->getSourceOrNull();
        if (syntax && syntax->kind == SyntaxKind::EnumType)
            getOrAddDeferredData().registerTransparentType(lastMember, *lazyType);
    }

    insertMember(&symbol, lastMember);
}

void Scope::addMembers(const SyntaxNode& syntax) {
    switch (syntax.kind) {
        case SyntaxKind::ModuleDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::ProgramDeclaration:
            compilation.addDefinition(syntax.as<ModuleDeclarationSyntax>(), *this);
            break;
        case SyntaxKind::PackageDeclaration:
            // Packages exist in their own namespace and are tracked in the Compilation
            compilation.addPackage(PackageSymbol::fromSyntax(compilation, syntax.as<ModuleDeclarationSyntax>()));
            break;
        case SyntaxKind::PackageImportDeclaration:
            for (auto item : syntax.as<PackageImportDeclarationSyntax>().items) {
                if (item->item.kind == TokenKind::Star) {
                    auto import = compilation.emplace<WildcardImportSymbol>(
                        item->package.valueText(),
                        item->item.location());

                    addMember(*import);
                    compilation.trackImport(importDataIndex, *import);
                }
                else {
                    addMember(*compilation.emplace<ExplicitImportSymbol>(
                        item->package.valueText(),
                        item->item.valueText(),
                        item->item.location()));
                }
            }
            break;
        case SyntaxKind::HierarchyInstantiation:
            addDeferredMember(syntax);
            break;
        case SyntaxKind::ModportDeclaration:
            // TODO: modports
            break;
        case SyntaxKind::IfGenerate:
        case SyntaxKind::LoopGenerate:
            // TODO: add special name conflict checks for generate blocks
            addDeferredMember(syntax);
            break;
        case SyntaxKind::FunctionDeclaration:
        case SyntaxKind::TaskDeclaration:
            addMember(SubroutineSymbol::fromSyntax(compilation, syntax.as<FunctionDeclarationSyntax>()));
            break;
        case SyntaxKind::DataDeclaration: {
            SmallVectorSized<const VariableSymbol*, 4> variables;
            VariableSymbol::fromSyntax(compilation, syntax.as<DataDeclarationSyntax>(), variables);
            for (auto variable : variables)
                addMember(*variable);
            break;
        }
        case SyntaxKind::ParameterDeclarationStatement: {
            SmallVectorSized<ParameterSymbol*, 16> params;
            ParameterSymbol::fromSyntax(compilation,
                                        syntax.as<ParameterDeclarationStatementSyntax>().parameter,
                                        params);
            for (auto param : params)
                addMember(*param);
            break;
        }
        case SyntaxKind::GenerateBlock:
            for (auto member : syntax.as<GenerateBlockSyntax>().members)
                addMembers(*member);
            break;
        case SyntaxKind::AlwaysBlock:
        case SyntaxKind::AlwaysCombBlock:
        case SyntaxKind::AlwaysLatchBlock:
        case SyntaxKind::AlwaysFFBlock:
        case SyntaxKind::InitialBlock:
        case SyntaxKind::FinalBlock: {
            const auto& blockSyntax = syntax.as<ProceduralBlockSyntax>();
            auto kind = SemanticFacts::getProceduralBlockKind(blockSyntax.kind);
            addMember(*compilation.emplace<ProceduralBlockSymbol>(compilation,
                                                                  blockSyntax.keyword.location(),
                                                                  kind));
            break;
        }
        default:
            THROW_UNREACHABLE;
    }
}

void Scope::lookup(string_view searchName, LookupResult& result) const {
    // First do a direct search and see if we find anything.
    ensureMembers();
    auto it = nameMap->find(searchName);
    if (it != nameMap->end()) {
        // If this is a local or scoped lookup, check that we can access
        // the symbol (it must be declared before usage). Callables can be
        // referenced anywhere in the scope, so the location doesn't matter for them.
        bool locationGood = true;
        const Symbol* symbol = it->second;
        if (result.referencePointMatters())
            locationGood = LookupRefPoint::before(*symbol) < result.referencePoint;

        if (locationGood) {
            // We found the symbol we wanted. If it was a wrapped symbol, unwrap it first.
            switch (symbol->kind) {
                case SymbolKind::ExplicitImport:
                    // TODO: handle missing package import symbol
                    result.setSymbol(*symbol->as<ExplicitImportSymbol>().importedSymbol(), true);
                    break;
                case SymbolKind::TransparentMember:
                    result.setSymbol(symbol->as<TransparentMemberSymbol>().wrapped);
                    break;
                default:
                    result.setSymbol(*symbol);
                    break;
            }
            return;
        }
    }

    // If we got here, we didn't find a viable symbol locally. Try looking in
    // any wildcard imports we may have.
    SmallVectorSized<std::tuple<const WildcardImportSymbol*, const Symbol*>, 4> importResults;
    for (auto import : compilation.queryImports(importDataIndex)) {
        if (result.referencePoint < LookupRefPoint::after(*import))
            break;

        // TODO: handle missing package
        auto symbol = import->getPackage()->lookupDirect(searchName);
        if (symbol) {
            importResults.append(std::make_tuple(import, symbol));
            result.addPotentialImport(*symbol);
        }
    }

    if (!importResults.empty()) {
        if (importResults.size() == 1)
            result.setSymbol(*std::get<1>(importResults[0]), true);
        return;
    }

    if (thisSym->kind == SymbolKind::Root) {
        // For scoped lookups, if we reach the root without finding anything,
        // look for a package.
        // TODO: handle missing package
        if (result.nameKind == LookupNameKind::Scoped)
            result.setSymbol(*compilation.getPackage(searchName));
        return;
    }

    // Continue up the scope chain.
    result.referencePoint = LookupRefPoint::after(asSymbol());
    return getParent()->lookup(searchName, result);
}

const Symbol* Scope::lookupDirect(string_view searchName) const {
    // If the parser added a missing identifier token, it already issued an
    // appropriate error. This check here makes it easier to silently continue
    // in that case without checking every time someone wants to do a lookup.
    if (searchName.empty())
        return nullptr;

    // Just do a simple lookup and return the result if we have one.
    // One wrinkle is that we should not include any imported symbols.
    ensureMembers();
    auto result = nameMap->find(searchName);
    if (result != nameMap->end() && result->second->kind != SymbolKind::ExplicitImport)
        return result->second;
    return nullptr;
}

Scope::DeferredMemberData& Scope::getOrAddDeferredData() {
    return compilation.getOrAddDeferredData(deferredMemberIndex);
}

void Scope::insertMember(const Symbol* member, const Symbol* at) const {
    ASSERT(!member->parentScope);
    ASSERT(!member->nextInScope);

    if (!at) {
        member->indexInScope = Symbol::Index{ 1 };
        member->nextInScope = std::exchange(firstMember, member);
    }
    else {
        member->indexInScope = Symbol::Index{ (uint32_t)at->indexInScope + (at == lastMember) };
        member->nextInScope = std::exchange(at->nextInScope, member);
    }

    if (!member->nextInScope)
        lastMember = member;

    member->parentScope = this;
    if (!member->name.empty())
        nameMap->emplace(member->name, member);
}

void Scope::addDeferredMember(const SyntaxNode& member) {
    getOrAddDeferredData().addMember(member, lastMember);
}

void Scope::realizeDeferredMembers() const {
    ASSERT(deferredMemberIndex != DeferredMemberIndex::Invalid);
    auto deferredData = compilation.getOrAddDeferredData(deferredMemberIndex);
    deferredMemberIndex = DeferredMemberIndex::Invalid;

    for (const auto& pair : deferredData.getTransparentTypes()) {
        const Symbol* insertAt = pair.first;
        const Type* type = pair.second->get();

        if (type && type->kind == SymbolKind::EnumType) {
            for (auto value : type->as<EnumType>().values()) {
                auto wrapped = compilation.emplace<TransparentMemberSymbol>(*value);
                insertMember(wrapped, insertAt);
                insertAt = wrapped;
            }
        }
    }

    if (deferredData.hasStatement()) {
        auto syntax = deferredData.getStatement();
        ASSERT(syntax);

        // The const_cast should always be safe here; there's no way for statement
        // syntax to be added to our deferred members unless the original class
        // was non-const.
        static_cast<StatementBodiedScope*>(const_cast<Scope*>(this))->bindBody(*syntax);
    }
    else {
        for (auto[node, insertionPoint] : deferredData.getMembers()) {
            switch (node->kind) {
                case SyntaxKind::HierarchyInstantiation: {
                    SmallVectorSized<const Symbol*, 8> symbols;
                    InstanceSymbol::fromSyntax(compilation, node->as<HierarchyInstantiationSyntax>(),
                                               *this, symbols);

                    const Symbol* last = insertionPoint;
                    for (auto symbol : symbols) {
                        insertMember(symbol, last);
                        last = symbol;
                    }
                    break;
                }
                case SyntaxKind::IfGenerate: {
                    auto block = GenerateBlockSymbol::fromSyntax(compilation,
                                                                 node->as<IfGenerateSyntax>(), *this);
                    if (block)
                        insertMember(block, insertionPoint);
                    break;
                }
                case SyntaxKind::LoopGenerate: {
                    const auto& block = GenerateBlockArraySymbol::fromSyntax(compilation,
                                                                             node->as<LoopGenerateSyntax>(),
                                                                             *this);
                    insertMember(&block, insertionPoint);
                    break;
                }
                default:
                    THROW_UNREACHABLE;
            }
        }
    }
}

}