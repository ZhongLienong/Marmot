#pragma once

#include <concepts>
#include <expected>
#include <functional>
#include <type_traits>
#include <utility>

namespace MarmotCompiler
{
	namespace Detail
	{
		template<typename E>
		struct FailureValue
		{
			E m_error;
		};
	}

	template<typename E>
	[[nodiscard]] constexpr auto Failure(E&& error)
	{
		return Detail::FailureValue<std::decay_t<E>>{ std::forward<E>(error) };
	}

	template<typename T, typename E>
	class Expected
	{
	public:
		using ValueType = T;

		constexpr Expected() = default;

		template<typename U>
			requires (!std::same_as<std::remove_cvref_t<U>, Expected> && std::is_constructible_v<T, U>)
		constexpr explicit(!std::is_convertible_v<U, T>) Expected(U&& value)
			: m_storage(std::in_place, std::forward<U>(value))
		{
		}

		template<typename G>
			requires std::is_constructible_v<E, G>
		constexpr explicit(!std::is_convertible_v<G, E>) Expected(Detail::FailureValue<G>&& failure)
			: m_storage(std::unexpect, std::move(failure.m_error))
		{
		}

		[[nodiscard]] constexpr bool HasValue() const noexcept { return m_storage.has_value(); }
		[[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

		constexpr decltype(auto) Value(this auto&& self)
		{
			return std::forward<decltype(self)>(self).m_storage.value();
		}

		[[nodiscard]] constexpr decltype(auto) Error(this auto&& self) noexcept
		{
			return std::forward<decltype(self)>(self).m_storage.error();
		}

		[[nodiscard]] constexpr decltype(auto) operator*(this auto&& self) noexcept
		{
			return *std::forward<decltype(self)>(self).m_storage;
		}

		[[nodiscard]] constexpr auto operator->(this auto&& self) noexcept
			requires (!std::is_void_v<T>)
		{
			return self.m_storage.operator->();
		}

		template<typename Fn>
		[[nodiscard]] constexpr auto AndThen(this auto&& self, Fn&& fn)
		{
			const auto invoke = [&]() -> decltype(auto)
			{
				if constexpr (std::is_void_v<T>)
				{
					return std::invoke(std::forward<Fn>(fn));
				}
				else
				{
					return std::invoke(std::forward<Fn>(fn), *std::forward<decltype(self)>(self));
				}
			};
			using Result = std::remove_cvref_t<decltype(invoke())>;
			static_assert(std::same_as<Result, Expected<typename Result::ValueType, E>>);
			if (self.HasValue())
			{
				return Result(invoke());
			}
			return Result(Failure(std::forward<decltype(self)>(self).Error()));
		}

		template<typename Fn>
		[[nodiscard]] constexpr auto OrElse(this auto&& self, Fn&& fn)
		{
			using Result = std::remove_cvref_t<std::invoke_result_t<Fn, decltype(std::forward<decltype(self)>(self).Error())>>;
			static_assert(std::same_as<Result, Expected<T, typename Result::ErrorType>>);
			if (!self.HasValue())
			{
				return Result(std::invoke(std::forward<Fn>(fn), std::forward<decltype(self)>(self).Error()));
			}
			if constexpr (std::is_void_v<T>)
			{
				return Result();
			}
			else
			{
				return Result(*std::forward<decltype(self)>(self));
			}
		}

		template<typename Fn>
		[[nodiscard]] constexpr auto Transform(this auto&& self, Fn&& fn)
		{
			auto transformed = std::forward<decltype(self)>(self).m_storage.transform(std::forward<Fn>(fn));
			return Expected<typename decltype(transformed)::value_type, E>(std::move(transformed));
		}

		template<typename Fn>
		[[nodiscard]] constexpr auto TransformError(this auto&& self, Fn&& fn)
		{
			auto transformed = std::forward<decltype(self)>(self).m_storage.transform_error(std::forward<Fn>(fn));
			return Expected<T, typename decltype(transformed)::error_type>(std::move(transformed));
		}

	private:
		using ErrorType = E;

		template<typename, typename>
		friend class Expected;

		constexpr explicit Expected(std::expected<T, E>&& storage)
			: m_storage(std::move(storage))
		{
		}

		std::expected<T, E> m_storage;
	};
}
