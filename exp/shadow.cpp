// A local variable hides a previous local declaration (MSVC C4456, level 4).
int sumOfOffsets (int n)
{
	const int offset = n + 1;
	int sum = 0;
	for (int i = 0; i < n; ++i)
	{
		const int offset = i * 2;
		sum += offset;
	}
	return sum + offset;
}
