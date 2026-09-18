#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

class MidoriType
{
public:
	struct UndecidedType {};

	struct GenericParam
	{
		std::string m_name;

		GenericParam(const std::string& name);
		bool operator==(const GenericParam& other) const;
	};

	struct TypeVariable
	{
		int m_id;

		TypeVariable(int id);
		bool operator==(const TypeVariable& other) const;
	};

	struct FloatType {};
	struct IntegerType {};
	struct ByteType {};
	struct WordType {};
	struct TextType {};
	struct BoolType {};
	struct UnitType {};
	struct NeverType {};

	struct ArrayType
	{
		std::shared_ptr<MidoriType> m_element_type;
	};

	struct WorkerType
	{
		std::shared_ptr<MidoriType> m_result_type;
	};

	struct ChannelType
	{
		std::shared_ptr<MidoriType> m_element_type;
	};

	struct CellType
	{
		std::shared_ptr<MidoriType> m_element_type;
	};

	struct RangeType
	{
		std::shared_ptr<MidoriType> m_element_type;
	};

	struct TupleType
	{
		std::vector<std::shared_ptr<MidoriType>> m_element_types;
	};

	struct ClassConstraint
	{
		// A where clause states one of two things: that a type belongs to a class,
		// or that an associated-type projection equals a type. The second form is
		// what lets a combinator transform element types, because it constrains
		// the projection while its type argument is still abstract.
		enum class Kind
		{
			Class,
			Equality
		};

		std::string m_class_name;
		std::vector<std::shared_ptr<MidoriType>> m_type_args;
		Kind m_kind = Kind::Class;
		std::shared_ptr<MidoriType> m_equality_lhs;
		std::shared_ptr<MidoriType> m_equality_rhs;

		ClassConstraint() = default;
		ClassConstraint(const std::string& typeclass_name, std::vector<std::shared_ptr<MidoriType>>&& type_args);
		ClassConstraint(std::shared_ptr<MidoriType>&& equality_lhs, std::shared_ptr<MidoriType>&& equality_rhs);
		bool operator==(const ClassConstraint& other) const;
		bool IsEquality() const;
	};

	struct AssociatedType
	{
		std::string m_class_name;
		std::string m_name;
		std::vector<std::shared_ptr<MidoriType>> m_type_args;

		AssociatedType() = default;
		AssociatedType(const std::string& class_name, const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& type_args);
		bool operator==(const AssociatedType& other) const;
	};

	struct FunctionType
	{
		std::vector<std::shared_ptr<MidoriType>> m_param_types;
		std::shared_ptr<MidoriType> m_return_type;
		std::vector<ClassConstraint> m_constraints;
		bool m_is_foreign = false;
	};

	struct StructType
	{
		std::vector<std::shared_ptr<MidoriType>> m_member_types;
		std::vector<std::string> m_member_names;
		std::string m_name;
		std::vector<std::string> m_generic_params;
		std::vector<std::shared_ptr<MidoriType>> m_type_arguments;
		std::vector<ClassConstraint> m_constraints;
		bool m_is_generic_instantiation = false;
	};

	struct UnionType
	{
		struct UnionMemberContext
		{
			std::vector<std::shared_ptr<MidoriType>> m_member_types;
			int m_tag;
		};

		std::unordered_map<std::string, UnionMemberContext> m_member_info;
		std::string m_name;
		std::vector<std::string> m_generic_params;
		std::vector<std::shared_ptr<MidoriType>> m_type_arguments;
		std::vector<ClassConstraint> m_constraints;
		bool m_is_generic_instantiation = false;

		UnionType(const std::string& name);
	};

	struct NewType
	{
		std::string m_name;
		std::shared_ptr<MidoriType> m_representation;
		std::vector<std::string> m_generic_params;
		std::vector<std::shared_ptr<MidoriType>> m_type_arguments;
		std::vector<ClassConstraint> m_constraints;
		bool m_is_generic_instantiation = false;
	};

	using MidoriTypeUnion = std::variant
	<
		UndecidedType,
		GenericParam,
		TypeVariable,
		FloatType,
		IntegerType,
		ByteType,
		WordType,
		TextType,
		BoolType,
		UnitType,
		NeverType,
		ArrayType,
		WorkerType,
		ChannelType,
		CellType,
		RangeType,
		TupleType,
		FunctionType,
		StructType,
		UnionType,
		NewType,
		AssociatedType,
		ClassConstraint
	>;

	MidoriTypeUnion m_type;

	MidoriType(MidoriTypeUnion&& actual_type);

public:
	template<typename T>
	constexpr bool IsType() const
	{
		return std::holds_alternative<T>(m_type);
	}

	template<typename T>
	T& GetType()
	{
		return std::get<T>(m_type);
	}

	template<typename T>
	const T& GetType() const
	{
		return std::get<T>(m_type);
	}

	template<typename T>
	requires std::is_same_v<T, FloatType> ||
	         std::is_same_v<T, IntegerType> ||
	         std::is_same_v<T, ByteType> ||
	         std::is_same_v<T, WordType> ||
	         std::is_same_v<T, BoolType> ||
	         std::is_same_v<T, TextType> ||
	         std::is_same_v<T, UnitType> ||
	         std::is_same_v<T, NeverType>
	static const std::shared_ptr<MidoriType>& MakeLiteralType()
	{
		static std::shared_ptr<MidoriType> s_literal_type = std::make_shared<MidoriType>(T{});
		return s_literal_type;
	}

	static const std::shared_ptr<MidoriType> MakeUndecidedType();
	static std::shared_ptr<MidoriType> MakeGenericType(const std::string& name);
	static std::shared_ptr<MidoriType> MakeTypeVariable(int id);
	static std::shared_ptr<MidoriType> MakeAssociatedType(const std::string& class_name, const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& type_args);
	static std::shared_ptr<MidoriType> MakeArrayType(const std::shared_ptr<MidoriType>& element_type);
	static std::shared_ptr<MidoriType> MakeWorkerType(const std::shared_ptr<MidoriType>& result_type);
	static std::shared_ptr<MidoriType> MakeChannelType(const std::shared_ptr<MidoriType>& element_type);
	static std::shared_ptr<MidoriType> MakeCellType(const std::shared_ptr<MidoriType>& element_type);
	static std::shared_ptr<MidoriType> MakeRangeType(const std::shared_ptr<MidoriType>& element_type);
	static std::shared_ptr<MidoriType> MakeTupleType(std::vector<std::shared_ptr<MidoriType>>&& element_types);
	static std::shared_ptr<MidoriType> MakeFunctionType(const std::vector<std::shared_ptr<MidoriType>>& param_types, std::shared_ptr<MidoriType>&& return_type, bool is_foreign = false);
	static std::shared_ptr<MidoriType> MakeStructType(const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& member_types, std::vector<std::string>&& member_names, std::vector<std::string>&& generic_params = {});
	static std::shared_ptr<MidoriType> MakeUnionType(const std::string& name, std::vector<std::string>&& generic_params = {});
	static std::shared_ptr<MidoriType> MakeNewType(const std::string& name, const std::shared_ptr<MidoriType>& representation, std::vector<std::string>&& generic_params = {});

	static std::shared_ptr<MidoriType> SubstituteTypeParams(const std::shared_ptr<MidoriType>& type, const std::unordered_map<std::string, std::shared_ptr<MidoriType>>& substitutions);

	using TypeArgumentSubstituteFn = std::function<std::shared_ptr<MidoriType>(const std::shared_ptr<MidoriType>&)>;

	static std::vector<std::shared_ptr<MidoriType>> InstantiateTypeArguments(const std::vector<std::string>& generic_params, const std::vector<std::shared_ptr<MidoriType>>& type_arguments, const TypeArgumentSubstituteFn& substitute);

	std::string ToString() const;

	static std::string MangleInstanceMethodName(const std::string& method_name, const std::string& typeclass_name, const std::vector<std::shared_ptr<MidoriType>>& type_args);

	static std::string DemangleInstanceMethodName(const std::string& mangled_name, const std::string& typeclass_name);

	bool IsNumericType() const;

	friend bool operator==(const MidoriType& lhs, const MidoriType& rhs);

private:
	struct TypeEqualityVisitor;
	static bool CompareStructTypes(const StructType& a, const StructType& b);
	static bool CompareUnionTypes(const UnionType& a, const UnionType& b);
	static bool CompareGenericStructs(const StructType& a, const StructType& b);
	static bool CompareGenericUnions(const UnionType& a, const UnionType& b);
	static bool CompareInstantiatedStructs(const StructType& a, const StructType& b);
	static bool CompareInstantiatedUnions(const UnionType& a, const UnionType& b);
	static bool CompareTypeArguments(const std::vector<std::shared_ptr<MidoriType>>& a, const std::vector<std::shared_ptr<MidoriType>>& b);
};

bool operator==(const MidoriType& lhs, const MidoriType& rhs);
