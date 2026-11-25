#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace MarmotCompiler
{
	struct NoneType {};
	inline constexpr NoneType None;

	template<typename T>
	class Optional
	{
	public:
		constexpr Optional() noexcept = default;
		constexpr Optional(NoneType) noexcept {}

		template<typename U>
			requires (!std::same_as<std::remove_cvref_t<U>, Optional> && std::is_constructible_v<T, U>)
		constexpr explicit(!std::is_convertible_v<U, T>) Optional(U&& value)
			: m_storage(std::in_place, std::forward<U>(value))
		{
		}

		[[nodiscard]] constexpr bool HasValue() const noexcept { return m_storage.has_value(); }
		[[nodiscard]] constexpr explicit operator bool() const noexcept { return HasValue(); }

		[[nodiscard]] constexpr decltype(auto) Value(this auto&& self)
		{
			return std::forward<decltype(self)>(self).m_storage.value();
		}

		[[nodiscard]] constexpr decltype(auto) operator*(this auto&& self) noexcept
		{
			return *std::forward<decltype(self)>(self).m_storage;
		}

		[[nodiscard]] constexpr auto operator->(this auto&& self) noexcept
		{
			return self.m_storage.operator->();
		}

		template<typename U>
		[[nodiscard]] constexpr T ValueOr(this auto&& self, U&& fallback)
		{
			return std::forward<decltype(self)>(self).m_storage.value_or(std::forward<U>(fallback));
		}

		template<typename... Args>
		constexpr T& Emplace(Args&&... args)
		{
			return m_storage.emplace(std::forward<Args>(args)...);
		}

		constexpr void Reset() noexcept { m_storage.reset(); }

		template<typename Fn>
			requires std::same_as<std::invoke_result_t<Fn>, Optional>
		[[nodiscard]] constexpr Optional OrElse(this auto&& self, Fn&& fn)
		{
			if (self.HasValue())
			{
				return std::forward<decltype(self)>(self);
			}
			return std::invoke(std::forward<Fn>(fn));
		}

		template<typename Fn>
		[[nodiscard]] constexpr auto Transform(this auto&& self, Fn&& fn)
		{
			auto transformed = std::forward<decltype(self)>(self).m_storage.transform(std::forward<Fn>(fn));
			return Optional<typename decltype(transformed)::value_type>(std::move(transformed));
		}

		friend constexpr bool operator==(const Optional&, const Optional&) = default;

	private:
		template<typename>
		friend class Optional;

		constexpr explicit Optional(std::optional<T>&& storage)
			: m_storage(std::move(storage))
		{
		}

		std::optional<T> m_storage;
	};

	template<typename T>
	Optional(T) -> Optional<T>;
}
