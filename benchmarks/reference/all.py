import time
from typing import List

def test_text_concatenation(iterations: int) -> None:
    start = time.perf_counter()
    result = ""
    i = 0
    while True:
        if i >= iterations:
            break
        else:
            result = result + "x"
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Text concatenation ({iterations} iterations): {duration_ms:.2f} ms")

def test_text_append(iterations: int) -> None:
    start = time.perf_counter()
    result = ""
    i = 0
    while True:
        if i >= iterations:
            break
        else:
            result += "x"
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Text append ({iterations} iterations): {duration_ms:.2f} ms")

def test_text_building(iterations: int) -> None:
    start = time.perf_counter()
    result = ""
    i = 0
    while True:
        if i >= iterations:
            break
        else:
            result = result + "Hello" + " " + "World" + " " + str(i)
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Text building ({iterations} iterations): {duration_ms:.2f} ms")

def test_array_append(size: int) -> None:
    start = time.perf_counter()
    arr: List[int] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr.append(i)
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array append ({size} elements): {duration_ms:.2f} ms")

def test_array_concatenation(size: int) -> None:
    start = time.perf_counter()
    arr: List[int] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr = arr + [i]
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array concatenation ({size} elements): {duration_ms:.2f} ms")

def test_array_access(size: int, num_accesses: int) -> None:
    arr: List[int] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr.append(i)
            i += 1

    start = time.perf_counter()
    total = 0
    j = 0
    while True:
        if j >= num_accesses:
            break
        else:
            index = j % size
            total = total + arr[index]
            j += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array access ({num_accesses} accesses, sum={total}): {duration_ms:.2f} ms")

def test_array_iteration(size: int) -> None:
    arr: List[int] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr.append(i)
            i += 1

    start = time.perf_counter()
    total = 0
    j = 0
    while True:
        if j >= size:
            break
        else:
            total = total + arr[j]
            j += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array iteration ({size} elements, sum={total}): {duration_ms:.2f} ms")

def test_array_of_text(size: int) -> None:
    start = time.perf_counter()
    arr: List[str] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr.append("Item " + str(i))
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array of Text ({size} elements): {duration_ms:.2f} ms")

def test_nested_arrays(outer_size: int, inner_size: int) -> None:
    start = time.perf_counter()
    outer: List[List[int]] = []
    i = 0
    while True:
        if i >= outer_size:
            break
        else:
            inner: List[int] = []
            j = 0
            while True:
                if j >= inner_size:
                    break
                else:
                    inner.append(i * inner_size + j)
                    j += 1
            outer.append(inner)
            i += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Nested arrays ({outer_size}x{inner_size}): {duration_ms:.2f} ms")

def test_array_filter(size: int) -> None:
    arr: List[int] = []
    i = 0
    while True:
        if i >= size:
            break
        else:
            arr.append(i)
            i += 1

    start = time.perf_counter()
    filtered: List[int] = []
    j = 0
    while True:
        if j >= size:
            break
        else:
            if arr[j] % 2 == 0:
                filtered.append(arr[j])
            j += 1
    end = time.perf_counter()
    duration_ms = (end - start) * 1000
    print(f"Array filter (even from {size} elements): {duration_ms:.2f} ms")

def test_prime_numbers() -> None:
    """
    Counts all prime numbers up to 100,000 and benchmarks the time.
    """
    
    def is_prime(n: int) -> bool:
        """Checks if a number is prime using a helper."""
        
        def is_prime_helper(n: int, divisor: int) -> bool:
            """Recursive helper to check for divisors."""
            if divisor * divisor > n:
                return True
            else:
                if n % divisor == 0:
                    return False
                else:
                    return is_prime_helper(n, divisor + 1)

        if n <= 1:
            return False
        else:
            return is_prime_helper(n, 2)

    start = time.perf_counter()

    prime_count = 0
    count = 0
    while True:
        if count >= 100_000:
            break
        else:
            if is_prime(count):
                prime_count += 1
            count += 1

    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"Prime(100000): {prime_count} benchmark took {duration_ms:.2f} milliseconds")

# ---

def test_fib(n: int) -> None:
    """
    Calculates the n-th Fibonacci number recursively and benchmarks the time.
    """
    
    def fib(n: int) -> int:
        """Recursive Fibonacci calculation."""
        if n <= 1:
            return n
        else:
            return fib(n - 1) + fib(n - 2)

    start = time.perf_counter()
    fib_n = fib(n)
    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"Fibonacci({n}): {fib_n} benchmark took {duration_ms:.2f} milliseconds")

# ---

def test_string_manipulation() -> None:
    """
    Benchmarks concatenating a string in a loop.
    """
    start = time.perf_counter()

    test_string = ""
    count = 0
    while True:
        if count >= 10_000:
            break
        else:
            # Note: This is an inefficient O(n^2) operation in Python,
            # preserved to match the intent of the original benchmark.
            test_string += "a"
            count += 1

    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"String manipulation benchmark took {duration_ms:.2f} milliseconds")

# ---

def test_parenthesis_generation(max_len: int) -> None:
    """
    Generates all valid parenthesis combinations for max_len pairs
    and benchmarks the time.
    """
    output: List[str] = []

    def backtrack(length: int, left: int, right: int, acc: str) -> None:
        """
        Recursive backtracking function to generate parenthesis.
        'length' is the target number of pairs.
        'left' and 'right' are the current counts of open/close parens.
        'acc' is the string being built.
        """
        if left < right:
            return
        else:
            if left > length or right > length:
                return
            else:
                if left == length and right == length:
                    output.append(acc)
                    return
                else:
                    backtrack(length, left + 1, right, acc + "(")
                    backtrack(length, left, right + 1, acc + ")")

    start = time.perf_counter()

    backtrack(max_len, 0, 0, "")

    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"Parenthesis generation benchmark took {duration_ms:.2f} milliseconds")
    # Uncomment the line below to see the count of combinations found
    # print(f"Found {len(output)} combinations.")

# ---

def test_ackermann(m: int, n: int) -> None:
    """
    Computes the Ackermann function recursively and benchmarks the time.
    """

    def ackermann(m: int, n: int) -> int:
        """Recursive Ackermann function."""
        if m == 0:
            return n + 1
        else:
            if n == 0:
                return ackermann(m - 1, 1)
            else:
                return ackermann(m - 1, ackermann(m, n - 1))

    start = time.perf_counter()
    result = ackermann(m, n)
    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"Ackermann({m}, {n}): {result} benchmark took {duration_ms:.2f} milliseconds")

# ---

def test_sorting(size: int) -> None:
    """
    Sorts an array using quicksort and benchmarks the time.
    """

    def quicksort(arr: List[int]) -> List[int]:
        """Quicksort implementation."""
        if len(arr) <= 1:
            return arr
        else:
            pivot = arr[len(arr) // 2]
            less = []
            equal = []
            greater = []

            for val in arr:
                if val < pivot:
                    less.append(val)
                elif val == pivot:
                    equal.append(val)
                else:
                    greater.append(val)

            return quicksort(less) + equal + quicksort(greater)

    start = time.perf_counter()

    # Create array in reverse order
    test_array = list(range(size, 0, -1))
    sorted_array = quicksort(test_array)

    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"QuickSort({size} elements): benchmark took {duration_ms:.2f} milliseconds")

# ---

def test_binary_search(array_size: int, num_searches: int) -> None:
    """
    Performs binary search operations and benchmarks the time.
    """

    def binary_search(arr: List[int], target: int, low: int, high: int) -> int:
        """Recursive binary search."""
        if low > high:
            return -1
        else:
            mid = low + (high - low) // 2
            mid_val = arr[mid]

            if mid_val == target:
                return mid
            else:
                if mid_val > target:
                    return binary_search(arr, target, low, mid - 1)
                else:
                    return binary_search(arr, target, mid + 1, high)

    start = time.perf_counter()

    # Create sorted array
    sorted_array = list(range(array_size))

    found_count = 0
    for j in range(num_searches):
        target = (j * 7) % array_size
        result = binary_search(sorted_array, target, 0, len(sorted_array) - 1)
        if result != -1:
            found_count += 1

    end = time.perf_counter()
    duration_ms = (end - start) * 1000

    print(f"Binary search ({num_searches} searches in {array_size} elements): {found_count} found, benchmark took {duration_ms:.2f} milliseconds")

# ---

if __name__ == "__main__":
    test_fib(35)
    test_prime_numbers()
    test_string_manipulation()
    test_parenthesis_generation(8)
    test_ackermann(3, 5)
    test_sorting(5000)
    test_binary_search(100000, 10000)