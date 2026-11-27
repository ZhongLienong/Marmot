#include <catch2/catch_test_macros.hpp>

#include "Compiler/Module/ModuleExports.h"
#include "Compiler/Lowering/Lowering.h"
#include "support/CompileHelpers.h"
#include "Support/Expected.h"

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	ModuleExport Export(std::string name, int line = 2)
	{
		return ModuleExport(Token(std::move(name), Token::Name::IDENTIFIER_LITERAL, line, "Facade.mmt"), VisibilityLevel::Public);
	}
}

TEST_CASE("Imported access and re-export candidates share namespace visibility", "[module][publication]")
{
	const ModuleExportMetadata::SymbolTable symbols = ModuleExportMetadata::SymbolTable()
		.WithExport("Open", VisibilityLevel::Public)
		.WithExport("Shared", VisibilityLevel::Private);
	const std::vector<std::shared_ptr<const ModuleExportMetadata>> dependencies
	{
		std::make_shared<const ModuleExportMetadata>("Library.Zulu", "Zulu.mmt", symbols, ModuleExportMetadata::TypeEnvironment(), TypeclassMetadataMap(), GenericFunctions()),
		std::make_shared<const ModuleExportMetadata>("Library.Alpha", "Alpha.mmt", symbols, ModuleExportMetadata::TypeEnvironment(), TypeclassMetadataMap(), GenericFunctions())
	};
	const MarmotCompiler::Expected<ModuleImports, ModuleImportConflict> imports = ModuleImports::Collect(dependencies);
	REQUIRE(imports.HasValue());
	CHECK(imports->AccessFrom("App.Main", "Library.Alpha", "Open") == ImportedSymbolAccess::Accessible);
	CHECK(imports->AccessFrom("Library.Main", "Library.Alpha", "Shared") == ImportedSymbolAccess::Accessible);
	CHECK(imports->AccessFrom("App.Main", "Library.Alpha", "Shared") == ImportedSymbolAccess::PrivateInaccessible);
	CHECK(imports->AccessFrom("", "Library.Alpha", "Shared") == ImportedSymbolAccess::PrivateInaccessible);
	CHECK(imports->AccessFrom("Library.Main", "Library.Alpha", "Missing") == ImportedSymbolAccess::SymbolNotExported);
	CHECK(imports->AccessFrom("Library.Main", "Missing", "Shared") == ImportedSymbolAccess::ModuleNotFound);
	CHECK(imports->ReexportOrigins("App.Main", "Shared", {}).empty());
	CHECK(imports->ReexportOrigins("Library.Main", "Shared", {}) == std::vector<std::string>{ "Library.Alpha", "Library.Zulu" });
	const std::vector<UseImport> uses{ UseImport("Library.Zulu", "Shared") };
	CHECK(imports->ReexportOrigins("Library.Main", "Shared", uses) == std::vector<std::string>{ "Library.Zulu" });
	CHECK(imports->ReexportOrigins("App.Main", "Shared", uses).empty());
}

TEST_CASE("Publication keeps re-exported type and generic origins through another importer", "[module][publication]")
{
	MarmotResult::DiagnosticsResult<LoweredModule> origin = MarmotTest::LowerSnippetWithDiagnostics(R"(module Origin
public export { Copy }
def Copy = fn<T>(value: T) -> T => value;
)");
	REQUIRE(origin.HasValue());
	const std::shared_ptr<const MarmotExpression> body = origin->m_generic_functions.at("Copy").m_body;
	const TypeRef point = MarmotType::MakeStructType("Point", "Origin", {}, {});
	const ModuleExportMetadata::SymbolTable symbols = ModuleExportMetadata::SymbolTable()
		.WithExport("Point", VisibilityLevel::Public)
		.WithExport("Copy", VisibilityLevel::Public);
	const std::vector<std::shared_ptr<const ModuleExportMetadata>> dependencies
	{
		std::make_shared<const ModuleExportMetadata>("Origin", "Origin.mmt", symbols, ModuleExportMetadata::TypeEnvironment{ { "Point", point } }, TypeclassMetadataMap(), std::move(origin->m_generic_functions))
	};
	const MarmotCompiler::Expected<ModuleImports, ModuleImportConflict> imports = ModuleImports::Collect(dependencies);
	REQUIRE(imports.HasValue());
	const ModuleDeclaration declaration = ModuleDeclaration("Facade", "Facade.mmt")
		.WithExports({ Export("Point"), Export("Copy"), Export("Inferred") });
	ModuleExports exports(&declaration, "Facade.mmt", {}, {});
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	exports.RecordInferredTypes({ { "Inferred", integer }, { "Hidden", integer } });
	LoweredModule lowered(MarmotIRModule("Facade"));
	MarmotResult::Result<PublishedModuleExports> published = std::move(exports).Publish(lowered, imports.Value(), {}, {});
	REQUIRE(published.HasValue());
	CHECK(published->m_reexports == std::unordered_map<std::string, std::string>{ { "Point", "Origin" }, { "Copy", "Origin" } });
	CHECK(published->m_export_metadata->m_type_signatures.at("Point") == point);
	CHECK(published->m_export_metadata->m_type_signatures.at("Inferred") == integer);
	CHECK_FALSE(published->m_export_metadata->m_type_signatures.contains("Hidden"));
	CHECK(published->m_export_metadata->m_generic_functions.at("Copy").m_body == body);

	const std::vector<std::shared_ptr<const ModuleExportMetadata>> facade{ published->m_export_metadata };
	const MarmotCompiler::Expected<ModuleImports, ModuleImportConflict> downstream = ModuleImports::Collect(facade);
	REQUIRE(downstream.HasValue());
	CHECK(downstream->Types().at("Facade::Point") == point);
	CHECK(downstream->Types().at("Origin::Point") == point);
	CHECK(downstream->Generics().at("Facade::Copy").m_body == body);
	CHECK(downstream->Generics().at("Origin::Copy").m_body == body);
	CHECK_FALSE(downstream->Generics().contains("Copy"));
}

TEST_CASE("Publication reports the first invalid export in source order", "[module][publication]")
{
	const ModuleDeclaration declaration = ModuleDeclaration("Facade", "Facade.mmt")
		.WithExports({ Export("Zulu", 3), Export("Alpha", 4) });
	ModuleExports exports(&declaration, "Facade.mmt", {}, {});
	LoweredModule lowered(MarmotIRModule("Facade"));
	const ModuleImports imports;
	const MarmotResult::Result<PublishedModuleExports> published = std::move(exports).Publish(lowered, imports, {}, {});
	REQUIRE_FALSE(published.HasValue());
	CHECK(published.Error().m_code == CompilerErrorCode::ModuleMissingExportedSymbol);
	REQUIRE(published.Error().m_location.HasValue());
	CHECK(published.Error().m_location->m_line == 3);
	CHECK(published.Error().m_message.find("'Zulu'") != std::string::npos);
}
