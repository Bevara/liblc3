/*
 *  One libm symbol that liblc3 references and solver_minimal_1 does not
 *  export. A side module whose imports are not all resolved never instantiates
 *  at all - emscripten reports it by crashing inside its own
 *  reportUndefinedSymbols, which names nothing - so it has to be defined here.
 */

float fmaxf(float a, float b)
{
	/* IEEE semantics: a NaN operand loses to a real number. */
	if (a != a)
		return b;
	if (b != b)
		return a;
	return (a > b) ? a : b;
}
