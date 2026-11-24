#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <utility>

TEST_CASE("An invalid tuple index reports the projection and its caller", "[runtime][error]")
{
	// Tuple projections emitted by the compiler have statically checked indices.
	const bool negative = GENERATE(false, true);
	const VmOpCode index_op = negative ? VmOpCode::INT_MINUS_1 : VmOpCode::INT_2;
	constexpr VmOpCode zero{ 0u };
	VmExecutable executable;
	executable.AttachShapes({ {}, { 0u } });
	executable.SetFileName("TupleBounds.mmt");
	executable.AttachProcedureNames({ "main", "Project" });
	executable.AttachProcedures
	({
		VmBytecodeStream
		(
			{ VmOpCode::CALL_PROC_WIDE, VmOpCode{ 1u }, zero, zero, VmOpCode::HALT },
			{ { 9, 5 } },
			{}, {}
		),
		VmBytecodeStream
		(
			{
				VmOpCode::INT_0, VmOpCode::INT_1,
				VmOpCode::CREATE_TUPLE, VmOpCode{ 2u }, zero, zero, zero, zero,
				index_op, VmOpCode::GET_TUPLE, VmOpCode::RETURN
			},
			{ { 4, 9 }, { 5, 1 }, { 6, 1 } },
			{}, {}
		)
	});

	VirtualMachine vm(std::move(executable));
	const VirtualMachine::ExecuteResult result = vm.Execute();
	REQUIRE_FALSE(result.has_value());
	const RuntimeError& error = result.error();
	CHECK(error.m_code == RuntimeErrorCode::IndexOutOfBounds);
	CHECK(error.ExitCode() == 1);
	CHECK(error.m_message == (negative ? "Index out of bounds at index: -1." : "Index out of bounds at index: 2."));
	REQUIRE(error.m_location.has_value());
	CHECK(error.m_location->m_file_name == "TupleBounds.mmt");
	CHECK(error.m_location->m_line == 5);
	REQUIRE(error.m_stack.size() == 2);
	CHECK(error.m_stack[0].m_procedure_name == "Project");
	CHECK(error.m_stack[0].m_location.m_line == 5);
	CHECK(error.m_stack[1].m_procedure_name == "main");
	CHECK(error.m_stack[1].m_location.m_line == 9);
}
