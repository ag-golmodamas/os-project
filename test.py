def fibonacci(n):
    """Return the nth Fibonacci number."""
    if n <= 0:
        return 0
    elif n == 1:
        return 1
    else:
        return fibonacci(n-1) + fibonacci(n-2)

# Test the function
num_terms = 10
print(f"First {num_terms} Fibonacci numbers:")
for i in range(num_terms):
    print(f"F({i}) = {fibonacci(i)}")