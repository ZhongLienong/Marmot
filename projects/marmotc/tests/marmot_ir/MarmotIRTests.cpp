#include <catch2/catch_test_macros.hpp>

#include "Compiler/MarmotIR/MarmotIR.h"
#include "Compiler/MarmotIR/Builder/MarmotIRBuilder.h"
#include "Compiler/MarmotIR/Printer/MarmotIRPrinter.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	const TypeRef& IntType()
	{
		return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	}

	const TypeRef& FloatType()
	{
		return MarmotType::MakeLiteralType<MarmotType::FloatType>();
	}

	const TypeRef& UnitType()
	{
		return MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}

	// The plan's first example: a self tail call over an array, already a loop.
	MarmotIRModule SumModule()
	{
		MarmotIRModule module("Sum");
		MarmotIRFunction function("Sum$Int", IntType());
		MarmotIRBuilder builder(function);
		const MarmotIRBlockId loop = builder.CreateBlock();
		const MarmotIRBlockId body = builder.CreateBlock();
		const MarmotIRBlockId done = builder.CreateBlock();

		const MarmotIRValueId xs = builder.AddParameter(MarmotIRFunction::s_entry_block, MarmotType::MakeArrayType(IntType()), "xs");
		const MarmotIRValueId n = builder.Emit(MarmotIROp::ArrayLength, IntType(), { xs }, {}, "n");
		const MarmotIRValueId zero = builder.ConstInt(0, "zero");
		builder.Jump(loop, { zero, zero });

		const MarmotIRValueId i = builder.AddParameter(loop, IntType(), "i");
		const MarmotIRValueId acc = builder.AddParameter(loop, IntType(), "acc");
		builder.PositionAt(loop);
		const MarmotIRValueId c = builder.Binary(MarmotIROp::LtInt, i, n, "c");
		builder.Branch(c, MarmotIRSuccessor(body), MarmotIRSuccessor(done));

		builder.PositionAt(body);
		const MarmotIRValueId x = builder.Emit(MarmotIROp::ArrayGet, IntType(), { xs, i }, {}, "x");
		const MarmotIRValueId a2 = builder.Binary(MarmotIROp::AddInt, acc, x, "a2");
		const MarmotIRValueId one = builder.ConstInt(1, "one");
		const MarmotIRValueId i2 = builder.Binary(MarmotIROp::AddInt, i, one, "i2");
		builder.Jump(loop, { i2, a2 });

		builder.PositionAt(done).Return(acc);

		module.AddFunction(std::move(function));
		return module;
	}

	// A function returning Int whose body the test writes into its entry block.
	struct SingleFunction
	{
		MarmotIRModule m_module;
		MarmotIRFunction m_function;
		MarmotIRBuilder m_builder;

		explicit SingleFunction(TypeRef return_type = IntType())
			: m_module("Test"),
			m_function("f", std::move(return_type)),
			m_builder(m_function)
		{
		}

		std::vector<MarmotIRViolation> Verify()
		{
			m_module.m_functions.push_back(m_function);
			return MarmotIRVerifier(m_module).Verify();
		}
	};

	bool Reports(const std::vector<MarmotIRViolation>& violations, MarmotIRRule rule)
	{
		return std::ranges::any_of(violations, [rule](const MarmotIRViolation& violation) { return violation.m_rule == rule; });
	}

	bool ReportsOnly(const std::vector<MarmotIRViolation>& violations, MarmotIRRule rule)
	{
		return !violations.empty() && std::ranges::all_of(violations, [rule](const MarmotIRViolation& violation) { return violation.m_rule == rule; });
	}

	std::string Describe(const std::vector<MarmotIRViolation>& violations)
	{
		std::string text;
		for (const MarmotIRViolation& violation : violations)
		{
			text.append(violation.ToString());
			text.push_back('\n');
		}
		return text;
	}
}

TEST_CASE("The printer writes the Sum example in the documented form", "[marmot_ir]")
{
	const std::string expected =
		"module Sum\n"
		"\n"
		"fn Sum$Int(Array<Int>) -> Int\n"
		"bb0(xs: Array<Int>):\n"
		"  n: Int = ArrayLength xs\n"
		"  zero: Int = Const 0\n"
		"  jump bb1(zero, zero)\n"
		"bb1(i: Int, acc: Int):\n"
		"  c: Bool = LtInt i, n\n"
		"  branch c, bb2, bb3\n"
		"bb2:\n"
		"  x: Int = ArrayGet xs, i  !fault(Index)\n"
		"  a2: Int = AddInt acc, x\n"
		"  one: Int = Const 1\n"
		"  i2: Int = AddInt i, one\n"
		"  jump bb1(i2, a2)\n"
		"bb3:\n"
		"  return acc\n";

	REQUIRE(MarmotIRPrinter(SumModule()).Print() == expected);
}

TEST_CASE("The Sum example verifies", "[marmot_ir]")
{
	const std::vector<MarmotIRViolation> violations = MarmotIRVerifier(SumModule()).Verify();
	INFO(Describe(violations));
	REQUIRE(violations.empty());
}

TEST_CASE("The printer shows globals, unnamed values, constants, block parameters and effects", "[marmot_ir]")
{
	MarmotIRModule module("Main");
	const MarmotIRGlobalSlot total = module.ReserveGlobal("total", IntType());
	MarmotIRFunction function("Main$top", UnitType());
	MarmotIRBuilder builder(function);
	const MarmotIRBlockId done = builder.CreateBlock();
	const MarmotIRBlockId other = builder.CreateBlock();

	const MarmotIRValueId flag = builder.AddParameter(MarmotIRFunction::s_entry_block, MarmotType::MakeLiteralType<MarmotType::BoolType>());
	const MarmotIRValueId text = builder.ConstText("a \"b\"\n");
	builder.ConstFloat(2.0);
	const MarmotIRValueId seven = builder.ConstInt(7);
	builder.Emit(MarmotIROp::GlobalDefine, UnitType(), { seven }, total);
	builder.Emit(MarmotIROp::IntToText, MarmotType::MakeLiteralType<MarmotType::TextType>(), { seven });
	builder.Branch(flag, MarmotIRSuccessor(done), MarmotIRSuccessor(other, { text }));

	builder.PositionAt(done).Return(builder.ConstUnit("done"));
	builder.AddParameter(other, MarmotType::MakeLiteralType<MarmotType::TextType>(), "message");
	builder.PositionAt(other).Unreachable();

	module.m_top_level = module.AddFunction(std::move(function));

	const std::string expected =
		"module Main\n"
		"global @0 total: Int\n"
		"top-level Main$top\n"
		"\n"
		"fn Main$top(Bool) -> Unit\n"
		"bb0(%0: Bool):\n"
		"  %1: Text = Const \"a \\\"b\\\"\\n\"\n"
		"  %2: Float = Const 2.0\n"
		"  %3: Int = Const 7\n"
		"  %4: Unit = GlobalDefine @0, %3  !io\n"
		"  %5: Text = IntToText %3  !alloc\n"
		"  branch %0, bb1, bb2(%1)\n"
		"bb1:\n"
		"  done: Unit = Const\n"
		"  return done\n"
		"bb2(message: Text):\n"
		"  unreachable\n";

	REQUIRE(MarmotIRPrinter(module).Print() == expected);
}

TEST_CASE("Two values that share a name print apart", "[marmot_ir]")
{
	SingleFunction test;
	const MarmotIRValueId first = test.m_builder.ConstInt(1, "x");
	test.m_builder.ConstInt(2, "x");
	test.m_builder.Return(first);
	test.m_module.m_functions.push_back(test.m_function);

	const std::string printed = MarmotIRPrinter(test.m_module).PrintFunction(test.m_function);
	REQUIRE(printed.contains("x.0: Int = Const 1"));
	REQUIRE(printed.contains("x.1: Int = Const 2"));
	REQUIRE(printed.contains("return x.0"));
}

TEST_CASE("Rule 1: a block that does not end in a terminator", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.ConstInt(1);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::Terminator));
}

TEST_CASE("Rule 1: a terminator before the end of its block", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRValueId one = test.m_builder.ConstInt(1);
	test.m_builder.Return(one);
	test.m_builder.Return(one);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::Terminator));
}

TEST_CASE("Rule 1: a function with no blocks", "[marmot_ir][verifier]")
{
	MarmotIRModule module("Test");
	module.AddFunction(MarmotIRFunction("f", IntType()));

	REQUIRE(ReportsOnly(MarmotIRVerifier(module).Verify(), MarmotIRRule::Terminator));
}

TEST_CASE("Rule 2: a value used before it is defined in the same block", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRValueId one = test.m_builder.ConstInt(1);
	const MarmotIRValueId sum = test.m_builder.Binary(MarmotIROp::AddInt, one, one);
	test.m_builder.Return(sum);
	std::swap(test.m_function.m_blocks[0u].m_instructions[0u], test.m_function.m_blocks[0u].m_instructions[1u]);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::Dominance));
}

TEST_CASE("Rule 2: a value defined on one side of a branch and used at the join", "[marmot_ir][verifier]")
{
	SingleFunction test;
	MarmotIRBuilder& builder = test.m_builder;
	const MarmotIRBlockId left = builder.CreateBlock();
	const MarmotIRBlockId right = builder.CreateBlock();
	const MarmotIRBlockId join = builder.CreateBlock();

	builder.Branch(builder.ConstBool(true), MarmotIRSuccessor(left), MarmotIRSuccessor(right));
	const MarmotIRValueId only_left = builder.PositionAt(left).ConstInt(1);
	builder.Jump(join);
	builder.PositionAt(right).Jump(join);
	builder.PositionAt(join).Return(only_left);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::Dominance));
}

TEST_CASE("Rule 2: a value passed to the join is dominated through its parameter", "[marmot_ir][verifier]")
{
	SingleFunction test;
	MarmotIRBuilder& builder = test.m_builder;
	const MarmotIRBlockId left = builder.CreateBlock();
	const MarmotIRBlockId right = builder.CreateBlock();
	const MarmotIRBlockId join = builder.CreateBlock();
	const MarmotIRValueId merged = builder.AddParameter(join, IntType());

	builder.Branch(builder.ConstBool(true), MarmotIRSuccessor(left), MarmotIRSuccessor(right));
	builder.PositionAt(left).Jump(join, { builder.ConstInt(1) });
	builder.PositionAt(right).Jump(join, { builder.ConstInt(2) });
	builder.PositionAt(join).Return(merged);

	const std::vector<MarmotIRViolation> violations = test.Verify();
	INFO(Describe(violations));
	REQUIRE(violations.empty());
}

TEST_CASE("Rule 2: a use of a value that does not exist", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(MarmotIRValueId{ 42u });

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::Dominance));
}

TEST_CASE("Rule 2: a value defined twice", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRValueId one = test.m_builder.ConstInt(1);
	test.m_builder.ConstInt(2);
	test.m_builder.Return(one);
	test.m_function.m_blocks[0u].m_instructions[1u].m_result = one;

	REQUIRE(Reports(test.Verify(), MarmotIRRule::Dominance));
}

TEST_CASE("Rule 3: a jump with the wrong number of arguments", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRBlockId target = test.m_builder.CreateBlock();
	const MarmotIRValueId parameter = test.m_builder.AddParameter(target, IntType());
	test.m_builder.Jump(target);
	test.m_builder.PositionAt(target).Return(parameter);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::SuccessorArguments));
}

TEST_CASE("Rule 3: a jump argument of the wrong type", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRBlockId target = test.m_builder.CreateBlock();
	const MarmotIRValueId parameter = test.m_builder.AddParameter(target, IntType());
	test.m_builder.Jump(target, { test.m_builder.ConstFloat(1.5) });
	test.m_builder.PositionAt(target).Return(parameter);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::SuccessorArguments));
}

TEST_CASE("Rule 3: a jump to a block that does not exist", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Jump(MarmotIRBlockId{ 9u });

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::SuccessorArguments));
}

TEST_CASE("Rule 4: an operand of the wrong type", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRValueId sum = test.m_builder.Binary(MarmotIROp::AddInt, test.m_builder.ConstInt(1), test.m_builder.ConstFloat(1.0));
	test.m_builder.Return(sum);

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a constant that is not its type", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(test.m_builder.Emit(MarmotIROp::Const, IntType(), {}, 1.5));

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a return of the wrong type", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(test.m_builder.ConstBool(false));

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a call whose arguments do not match the callee", "[marmot_ir][verifier]")
{
	MarmotIRModule module("Test");
	MarmotIRFunction callee("g", IntType());
	MarmotIRBuilder callee_builder(callee);
	callee_builder.Return(callee_builder.AddParameter(MarmotIRFunction::s_entry_block, IntType()));
	const MarmotIRFunctionId g = module.AddFunction(std::move(callee));

	MarmotIRFunction caller("f", IntType());
	MarmotIRBuilder builder(caller);
	builder.Return(builder.Emit(MarmotIROp::Call, IntType(), { builder.ConstFloat(1.0) }, g));
	module.AddFunction(std::move(caller));

	REQUIRE(ReportsOnly(MarmotIRVerifier(module).Verify(), MarmotIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a tail call is checked against the caller's return type", "[marmot_ir][verifier]")
{
	MarmotIRModule module("Test");
	MarmotIRFunction callee("g", FloatType());
	MarmotIRBuilder callee_builder(callee);
	callee_builder.Return(callee_builder.ConstFloat(1.0));
	const MarmotIRFunctionId g = module.AddFunction(std::move(callee));

	MarmotIRFunction caller("f", IntType());
	MarmotIRBuilder(caller).TailCall(g, {});
	module.AddFunction(std::move(caller));

	REQUIRE(ReportsOnly(MarmotIRVerifier(module).Verify(), MarmotIRRule::OperandTypes));
}

namespace
{
	const TypeRef& NeverType()
	{
		return MarmotType::MakeLiteralType<MarmotType::NeverType>();
	}

	// A module holding `stop`, which returns Never, and a function `f`
	// returning Int whose body the test writes.
	struct NeverModule
	{
		MarmotIRModule m_module;
		MarmotIRFunctionId m_stop;
		MarmotIRFunction m_function;
		MarmotIRBuilder m_builder;

		NeverModule()
			: m_module("Test"),
			m_stop(m_module.AddFunction(MarmotIRFunction("stop", NeverType()))),
			m_function("f", IntType()),
			m_builder(m_function)
		{
			MarmotIRBuilder stop(m_module.Function(m_stop));
			stop.TailCall(m_stop, {});
		}

		std::vector<MarmotIRViolation> Verify()
		{
			m_module.AddFunction(m_function);
			return MarmotIRVerifier(m_module).Verify();
		}
	};
}

TEST_CASE("Rule 1: a call that returns Never is followed by unreachable", "[marmot_ir][verifier][never]")
{
	NeverModule valid;
	valid.m_builder.Emit(MarmotIROp::Call, NeverType(), {}, valid.m_stop);
	valid.m_builder.Unreachable();
	CHECK(valid.Verify().empty());

	NeverModule invalid;
	invalid.m_builder.Emit(MarmotIROp::Call, NeverType(), {}, invalid.m_stop);
	invalid.m_builder.Return(invalid.m_builder.ConstInt(1));
	CHECK(ReportsOnly(invalid.Verify(), MarmotIRRule::Terminator));
}

TEST_CASE("Rule 4: nothing uses a Never value", "[marmot_ir][verifier][never]")
{
	NeverModule test;
	const MarmotIRBlockId after = test.m_builder.CreateBlock();
	const MarmotIRValueId never = test.m_builder.Emit(MarmotIROp::Call, NeverType(), {}, test.m_stop);
	test.m_builder.Unreachable();
	test.m_builder.PositionAt(after).Return(never);

	// The block that would use it is one nothing reaches, so only its types
	// would be wrong, and those are not checked.
	CHECK(test.Verify().empty());
}

TEST_CASE("Rule 4: a tail call of a function that returns Never ends a function of any type", "[marmot_ir][verifier][never]")
{
	NeverModule test;
	test.m_builder.TailCall(test.m_stop, {});

	CHECK(test.Verify().empty());
}

TEST_CASE("Rule 4: a closure leaves out the captures BindCaptures fills, at the end", "[marmot_ir][verifier][closure]")
{
	MarmotIRModule module("Test");
	const MarmotIRFunctionId lambda = module.AddFunction(MarmotIRFunction("lambda", IntType(), { IntType(), FloatType() }));
	MarmotIRBuilder lambda_builder(module.Function(lambda));
	lambda_builder.Return(lambda_builder.Emit(MarmotIROp::GetCapture, IntType(), {}, MarmotIRIndex{ 0u }));

	const TypeRef closure_type = MarmotType::MakeFunctionType({}, TypeRef(IntType()));
	MarmotIRFunction valid("valid", UnitType());
	MarmotIRBuilder valid_builder(valid);
	const MarmotIRValueId closure = valid_builder.Emit(MarmotIROp::MakeClosure, closure_type, { valid_builder.ConstInt(1) }, lambda);
	valid_builder.Emit(MarmotIROp::BindCaptures, UnitType(), { closure, valid_builder.ConstFloat(2.0) }, MarmotIRIndex{ 1u });
	valid_builder.Return(valid_builder.ConstUnit());
	module.AddFunction(std::move(valid));
	CHECK(MarmotIRVerifier(module).Verify().empty());

	MarmotIRFunction invalid("invalid", UnitType());
	MarmotIRBuilder invalid_builder(invalid);
	invalid_builder.Emit(MarmotIROp::MakeClosure, closure_type, { invalid_builder.ConstFloat(2.0) }, lambda);
	invalid_builder.Return(invalid_builder.ConstUnit());
	module.AddFunction(std::move(invalid));
	CHECK(ReportsOnly(MarmotIRVerifier(module).Verify(), MarmotIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a newtype is its representation", "[marmot_ir][verifier]")
{
	const TypeRef meters = MarmotType::MakeNewType("Meters", "Test", IntType());
	SingleFunction test;
	const MarmotIRValueId distance = test.m_builder.AddParameter(MarmotIRFunction::s_entry_block, meters);
	test.m_builder.Return(test.m_builder.Binary(MarmotIROp::AddInt, distance, test.m_builder.ConstInt(1)));

	CHECK(test.Verify().empty());
}

TEST_CASE("Rule 5: a read of a global slot that was never reserved", "[marmot_ir][verifier]")
{
	SingleFunction test;
	test.m_module.ReserveGlobal("total", IntType());
	test.m_builder.Return(test.m_builder.Emit(MarmotIROp::GlobalGet, IntType(), {}, MarmotIRGlobalSlot{ 1u }));

	REQUIRE(ReportsOnly(test.Verify(), MarmotIRRule::GlobalSlot));
}

TEST_CASE("Rule 5: a read of a reserved global slot is valid", "[marmot_ir][verifier]")
{
	SingleFunction test;
	const MarmotIRGlobalSlot total = test.m_module.ReserveGlobal("total", IntType());
	test.m_builder.Return(test.m_builder.Emit(MarmotIROp::GlobalGet, IntType(), {}, total));

	REQUIRE(test.Verify().empty());
}
