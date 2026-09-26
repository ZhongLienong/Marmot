#include <catch2/catch_test_macros.hpp>

#include "Compiler/MidoriIR/MidoriIR.h"
#include "Compiler/MidoriIR/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/MidoriIRPrinter.h"
#include "Compiler/MidoriIR/MidoriIRVerifier.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	const TypeRef& IntType()
	{
		return MidoriType::MakeLiteralType<MidoriType::IntegerType>();
	}

	const TypeRef& FloatType()
	{
		return MidoriType::MakeLiteralType<MidoriType::FloatType>();
	}

	const TypeRef& UnitType()
	{
		return MidoriType::MakeLiteralType<MidoriType::UnitType>();
	}

	// union Shape = Circle(Float) | Square(Float) | Empty
	TypeRef ShapeType()
	{
		TypeRef shape = MidoriType::MakeUnionType("Shape", "Test");
		MidoriType::UnionType& union_type = shape->GetType<MidoriType::UnionType>();
		union_type.m_member_info.emplace("Circle", MidoriType::UnionType::UnionMemberContext{ { FloatType() }, 0 });
		union_type.m_member_info.emplace("Square", MidoriType::UnionType::UnionMemberContext{ { FloatType() }, 1 });
		union_type.m_member_info.emplace("Empty", MidoriType::UnionType::UnionMemberContext{ {}, 2 });
		return shape;
	}

	// The plan's first example: a self tail call over an array, already a loop.
	MidoriIRModule SumModule()
	{
		MidoriIRModule module("Sum");
		MidoriIRFunction function("Sum$Int", IntType());
		MidoriIRBuilder builder(function);
		const MidoriIRBlockId loop = builder.CreateBlock();
		const MidoriIRBlockId body = builder.CreateBlock();
		const MidoriIRBlockId done = builder.CreateBlock();

		const MidoriIRValueId xs = builder.AddParameter(MidoriIRFunction::s_entry_block, MidoriType::MakeArrayType(IntType()), "xs");
		const MidoriIRValueId n = builder.Emit(MidoriIROp::ArrayLength, IntType(), { xs }, {}, "n");
		const MidoriIRValueId zero = builder.ConstInt(0, "zero");
		builder.Jump(loop, { zero, zero });

		const MidoriIRValueId i = builder.AddParameter(loop, IntType(), "i");
		const MidoriIRValueId acc = builder.AddParameter(loop, IntType(), "acc");
		builder.PositionAt(loop);
		const MidoriIRValueId c = builder.Binary(MidoriIROp::LtInt, i, n, "c");
		builder.Branch(c, MidoriIRSuccessor(body), MidoriIRSuccessor(done));

		builder.PositionAt(body);
		const MidoriIRValueId x = builder.Emit(MidoriIROp::ArrayGet, IntType(), { xs, i }, {}, "x");
		const MidoriIRValueId a2 = builder.Binary(MidoriIROp::AddInt, acc, x, "a2");
		const MidoriIRValueId one = builder.ConstInt(1, "one");
		const MidoriIRValueId i2 = builder.Binary(MidoriIROp::AddInt, i, one, "i2");
		builder.Jump(loop, { i2, a2 });

		builder.PositionAt(done).Return(acc);

		module.AddFunction(std::move(function));
		return module;
	}

	// A function returning Int whose body the test writes into its entry block.
	struct SingleFunction
	{
		MidoriIRModule m_module;
		MidoriIRFunction m_function;
		MidoriIRBuilder m_builder;

		explicit SingleFunction(TypeRef return_type = IntType())
			: m_module("Test"),
			m_function("f", std::move(return_type)),
			m_builder(m_function)
		{
		}

		std::vector<MidoriIRViolation> Verify()
		{
			m_module.m_functions.push_back(m_function);
			return MidoriIRVerifier(m_module).Verify();
		}
	};

	bool Reports(const std::vector<MidoriIRViolation>& violations, MidoriIRRule rule)
	{
		return std::ranges::any_of(violations, [rule](const MidoriIRViolation& violation) { return violation.m_rule == rule; });
	}

	bool ReportsOnly(const std::vector<MidoriIRViolation>& violations, MidoriIRRule rule)
	{
		return !violations.empty() && std::ranges::all_of(violations, [rule](const MidoriIRViolation& violation) { return violation.m_rule == rule; });
	}

	std::string Describe(const std::vector<MidoriIRViolation>& violations)
	{
		std::string text;
		for (const MidoriIRViolation& violation : violations)
		{
			text.append(violation.ToString());
			text.push_back('\n');
		}
		return text;
	}
}

TEST_CASE("The printer writes the Sum example in the documented form", "[midori_ir]")
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

	REQUIRE(MidoriIRPrinter(SumModule()).Print() == expected);
}

TEST_CASE("The Sum example verifies", "[midori_ir]")
{
	const std::vector<MidoriIRViolation> violations = MidoriIRVerifier(SumModule()).Verify();
	INFO(Describe(violations));
	REQUIRE(violations.empty());
}

TEST_CASE("The printer shows globals, unnamed values, constants, switches and effects", "[midori_ir]")
{
	MidoriIRModule module("Main");
	const MidoriIRGlobalSlot total = module.ReserveGlobal("total", IntType());
	MidoriIRFunction function("Main$top", UnitType());
	MidoriIRBuilder builder(function);
	const MidoriIRBlockId circle = builder.CreateBlock();
	const MidoriIRBlockId other = builder.CreateBlock();

	const MidoriIRValueId shape = builder.AddParameter(MidoriIRFunction::s_entry_block, ShapeType());
	const MidoriIRValueId text = builder.ConstText("a \"b\"\n");
	builder.ConstFloat(2.0);
	const MidoriIRValueId seven = builder.ConstInt(7);
	builder.Emit(MidoriIROp::GlobalDefine, UnitType(), { seven }, total);
	builder.Emit(MidoriIROp::IntToText, MidoriType::MakeLiteralType<MidoriType::TextType>(), { seven });
	builder.Switch(shape, { MidoriIRSuccessor(circle, {}, 0) }, MidoriIRSuccessor(other, { text }));

	builder.PositionAt(circle).Return(builder.ConstUnit("done"));
	builder.AddParameter(other, MidoriType::MakeLiteralType<MidoriType::TextType>(), "message");
	builder.PositionAt(other).Halt();

	module.m_top_level = module.AddFunction(std::move(function));

	const std::string expected =
		"module Main\n"
		"global @0 total: Int\n"
		"top-level Main$top\n"
		"\n"
		"fn Main$top(Test::Shape) -> Unit\n"
		"bb0(%0: Test::Shape):\n"
		"  %1: Text = Const \"a \\\"b\\\"\\n\"\n"
		"  %2: Float = Const 2.0\n"
		"  %3: Int = Const 7\n"
		"  %4: Unit = GlobalDefine @0, %3  !io\n"
		"  %5: Text = IntToText %3  !alloc\n"
		"  switch %0, 0: bb1, default: bb2(%1)\n"
		"bb1:\n"
		"  done: Unit = Const\n"
		"  return done\n"
		"bb2(message: Text):\n"
		"  halt\n";

	REQUIRE(MidoriIRPrinter(module).Print() == expected);
}

TEST_CASE("Two values that share a name print apart", "[midori_ir]")
{
	SingleFunction test;
	const MidoriIRValueId first = test.m_builder.ConstInt(1, "x");
	test.m_builder.ConstInt(2, "x");
	test.m_builder.Return(first);
	test.m_module.m_functions.push_back(test.m_function);

	const std::string printed = MidoriIRPrinter(test.m_module).PrintFunction(test.m_function);
	REQUIRE(printed.contains("x.0: Int = Const 1"));
	REQUIRE(printed.contains("x.1: Int = Const 2"));
	REQUIRE(printed.contains("return x.0"));
}

TEST_CASE("Rule 1: a block that does not end in a terminator", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.ConstInt(1);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::Terminator));
}

TEST_CASE("Rule 1: a terminator before the end of its block", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRValueId one = test.m_builder.ConstInt(1);
	test.m_builder.Return(one);
	test.m_builder.Return(one);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::Terminator));
}

TEST_CASE("Rule 1: a function with no blocks", "[midori_ir][verifier]")
{
	MidoriIRModule module("Test");
	module.AddFunction(MidoriIRFunction("f", IntType()));

	REQUIRE(ReportsOnly(MidoriIRVerifier(module).Verify(), MidoriIRRule::Terminator));
}

TEST_CASE("Rule 2: a value used before it is defined in the same block", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRValueId one = test.m_builder.ConstInt(1);
	const MidoriIRValueId sum = test.m_builder.Binary(MidoriIROp::AddInt, one, one);
	test.m_builder.Return(sum);
	std::swap(test.m_function.m_blocks[0u].m_instructions[0u], test.m_function.m_blocks[0u].m_instructions[1u]);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::Dominance));
}

TEST_CASE("Rule 2: a value defined on one side of a branch and used at the join", "[midori_ir][verifier]")
{
	SingleFunction test;
	MidoriIRBuilder& builder = test.m_builder;
	const MidoriIRBlockId left = builder.CreateBlock();
	const MidoriIRBlockId right = builder.CreateBlock();
	const MidoriIRBlockId join = builder.CreateBlock();

	builder.Branch(builder.ConstBool(true), MidoriIRSuccessor(left), MidoriIRSuccessor(right));
	const MidoriIRValueId only_left = builder.PositionAt(left).ConstInt(1);
	builder.Jump(join);
	builder.PositionAt(right).Jump(join);
	builder.PositionAt(join).Return(only_left);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::Dominance));
}

TEST_CASE("Rule 2: a value passed to the join is dominated through its parameter", "[midori_ir][verifier]")
{
	SingleFunction test;
	MidoriIRBuilder& builder = test.m_builder;
	const MidoriIRBlockId left = builder.CreateBlock();
	const MidoriIRBlockId right = builder.CreateBlock();
	const MidoriIRBlockId join = builder.CreateBlock();
	const MidoriIRValueId merged = builder.AddParameter(join, IntType());

	builder.Branch(builder.ConstBool(true), MidoriIRSuccessor(left), MidoriIRSuccessor(right));
	builder.PositionAt(left).Jump(join, { builder.ConstInt(1) });
	builder.PositionAt(right).Jump(join, { builder.ConstInt(2) });
	builder.PositionAt(join).Return(merged);

	const std::vector<MidoriIRViolation> violations = test.Verify();
	INFO(Describe(violations));
	REQUIRE(violations.empty());
}

TEST_CASE("Rule 2: a use of a value that does not exist", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(MidoriIRValueId{ 42u });

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::Dominance));
}

TEST_CASE("Rule 2: a value defined twice", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRValueId one = test.m_builder.ConstInt(1);
	test.m_builder.ConstInt(2);
	test.m_builder.Return(one);
	test.m_function.m_blocks[0u].m_instructions[1u].m_result = one;

	REQUIRE(Reports(test.Verify(), MidoriIRRule::Dominance));
}

TEST_CASE("Rule 3: a jump with the wrong number of arguments", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRBlockId target = test.m_builder.CreateBlock();
	const MidoriIRValueId parameter = test.m_builder.AddParameter(target, IntType());
	test.m_builder.Jump(target);
	test.m_builder.PositionAt(target).Return(parameter);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::SuccessorArguments));
}

TEST_CASE("Rule 3: a jump argument of the wrong type", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRBlockId target = test.m_builder.CreateBlock();
	const MidoriIRValueId parameter = test.m_builder.AddParameter(target, IntType());
	test.m_builder.Jump(target, { test.m_builder.ConstFloat(1.5) });
	test.m_builder.PositionAt(target).Return(parameter);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::SuccessorArguments));
}

TEST_CASE("Rule 3: a jump to a block that does not exist", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Jump(MidoriIRBlockId{ 9u });

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::SuccessorArguments));
}

TEST_CASE("Rule 4: an operand of the wrong type", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRValueId sum = test.m_builder.Binary(MidoriIROp::AddInt, test.m_builder.ConstInt(1), test.m_builder.ConstFloat(1.0));
	test.m_builder.Return(sum);

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a constant that is not its type", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(test.m_builder.Emit(MidoriIROp::Const, IntType(), {}, 1.5));

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a return of the wrong type", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_builder.Return(test.m_builder.ConstBool(false));

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a call whose arguments do not match the callee", "[midori_ir][verifier]")
{
	MidoriIRModule module("Test");
	MidoriIRFunction callee("g", IntType());
	MidoriIRBuilder callee_builder(callee);
	callee_builder.Return(callee_builder.AddParameter(MidoriIRFunction::s_entry_block, IntType()));
	const MidoriIRFunctionId g = module.AddFunction(std::move(callee));

	MidoriIRFunction caller("f", IntType());
	MidoriIRBuilder builder(caller);
	builder.Return(builder.Emit(MidoriIROp::Call, IntType(), { builder.ConstFloat(1.0) }, g));
	module.AddFunction(std::move(caller));

	REQUIRE(ReportsOnly(MidoriIRVerifier(module).Verify(), MidoriIRRule::OperandTypes));
}

TEST_CASE("Rule 4: a tail call is checked against the caller's return type", "[midori_ir][verifier]")
{
	MidoriIRModule module("Test");
	MidoriIRFunction callee("g", FloatType());
	MidoriIRBuilder callee_builder(callee);
	callee_builder.Return(callee_builder.ConstFloat(1.0));
	const MidoriIRFunctionId g = module.AddFunction(std::move(callee));

	MidoriIRFunction caller("f", IntType());
	MidoriIRBuilder(caller).TailCall(g, {});
	module.AddFunction(std::move(caller));

	REQUIRE(ReportsOnly(MidoriIRVerifier(module).Verify(), MidoriIRRule::OperandTypes));
}

TEST_CASE("Rule 5: a switch with two cases for one tag", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRBlockId target = test.m_builder.CreateBlock();
	const MidoriIRValueId shape = test.m_builder.AddParameter(MidoriIRFunction::s_entry_block, ShapeType());
	test.m_builder.Switch(shape, { MidoriIRSuccessor(target, {}, 0), MidoriIRSuccessor(target, {}, 0) }, MidoriIRSuccessor(target));
	test.m_builder.PositionAt(target).Return(test.m_builder.ConstInt(0));

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::SwitchCoverage));
}

TEST_CASE("Rule 5: a switch with no default that misses a member", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRBlockId target = test.m_builder.CreateBlock();
	const MidoriIRValueId shape = test.m_builder.AddParameter(MidoriIRFunction::s_entry_block, ShapeType());
	test.m_builder.Switch(shape, { MidoriIRSuccessor(target, {}, 0), MidoriIRSuccessor(target, {}, 1) });
	test.m_builder.PositionAt(target).Return(test.m_builder.ConstInt(0));

	const std::vector<MidoriIRViolation> violations = test.Verify();
	REQUIRE(ReportsOnly(violations, MidoriIRRule::SwitchCoverage));
	REQUIRE(violations.front().m_message.contains("Empty"));
}

TEST_CASE("Rule 5: a switch covering every member, or with a default, is valid", "[midori_ir][verifier]")
{
	SingleFunction every_member;
	const MidoriIRBlockId target = every_member.m_builder.CreateBlock();
	const MidoriIRValueId shape = every_member.m_builder.AddParameter(MidoriIRFunction::s_entry_block, ShapeType());
	every_member.m_builder.Switch(shape, { MidoriIRSuccessor(target, {}, 0), MidoriIRSuccessor(target, {}, 1), MidoriIRSuccessor(target, {}, 2) });
	every_member.m_builder.PositionAt(target).Return(every_member.m_builder.ConstInt(0));
	REQUIRE(every_member.Verify().empty());

	SingleFunction with_default;
	const MidoriIRBlockId default_target = with_default.m_builder.CreateBlock();
	const MidoriIRValueId other_shape = with_default.m_builder.AddParameter(MidoriIRFunction::s_entry_block, ShapeType());
	with_default.m_builder.Switch(other_shape, { MidoriIRSuccessor(default_target, {}, 1) }, MidoriIRSuccessor(default_target));
	with_default.m_builder.PositionAt(default_target).Return(with_default.m_builder.ConstInt(0));
	REQUIRE(with_default.Verify().empty());
}

TEST_CASE("Rule 6: a read of a global slot that was never reserved", "[midori_ir][verifier]")
{
	SingleFunction test;
	test.m_module.ReserveGlobal("total", IntType());
	test.m_builder.Return(test.m_builder.Emit(MidoriIROp::GlobalGet, IntType(), {}, MidoriIRGlobalSlot{ 1u }));

	REQUIRE(ReportsOnly(test.Verify(), MidoriIRRule::GlobalSlot));
}

TEST_CASE("Rule 6: a read of a reserved global slot is valid", "[midori_ir][verifier]")
{
	SingleFunction test;
	const MidoriIRGlobalSlot total = test.m_module.ReserveGlobal("total", IntType());
	test.m_builder.Return(test.m_builder.Emit(MidoriIROp::GlobalGet, IntType(), {}, total));

	REQUIRE(test.Verify().empty());
}
