#include "value.h"
#include "basic.h"
#include "stack.h"

/*
The C function follows the rule ((C x) y) = x.  This is known as the "constancy
function", or "Konstanzfunktion" in the original German.
*/
value type_C(value f)
	{
	if (!f->L->L) return f;
	return f->L->R;
	}

/*
The S function follows the rule (((S x) y) z) = ((x z) (y z)).  This is known
as the "fusion function", or "Verschmelzungfunktion" in the original German.
*/
value type_S(value f)
	{
	if (!f->L->L || !f->L->L->L) return f;
	return A( A(f->L->L->R, f->R), A(f->L->R, f->R) );
	}

/*
The I function follows the rule (I x) = x.  This is known as the "identity
function."
*/
value type_I(value f)
	{
	return f->R;
	}

/*
The R function follows the rule (R x y z) = (x (y z)).  I call it R because it
passes z to the right side only.  It can be defined as
	S (C S) C
but it's such a common pattern that I make a special combinator for it.
*/
value type_R(value f)
	{
	if (!f->L->L || !f->L->L->L) return f;
	return A( f->L->L->R, A(f->L->R, f->R) );
	}

/*
The L function follows the rule (L x y z) = (x z y).  I call it L because it
passes z to the left side only.  It can be defined as
	S (S (C S) (S (C C) S)) (C C)
but it's such a common pattern that I make a special combinator for it.
*/
value type_L(value f)
	{
	if (!f->L->L || !f->L->L->L) return f;
	return A( A(f->L->L->R, f->R), f->L->R);
	}

/*
The Y function follows the rule (Y f) = (f (Y f)).  This is known as the
"fixpoint function."
*/
value type_Y(value f)
	{
	return A(f->R, A(f->L,f->R));
	}

/*
The F function follows the rule ((F x) y) = y.  It represents False, which
always returns its second argument. */
value type_F(value f)
	{
	if (!f->L->L) return f;
	return f->R;
	}

/*
The query function is used for eager evaluation.  It follows this rule:
  (query x y) = (y x)

However, it evaluates x first before passing it to y.

This function is called "?" in the standard context.
*/
value type_query(value f)
	{
	if (!f->L->L) return f;
	if (!f->L->R->T) push(f->L->R);
	return A(f->R,f->L->R);
	}

/*
(item head tail) is the list with first element head followed by the list tail.
It follows the rule (item h t f g) = (g h t), and can be defined as
	\item = (\head\tail \end\item item head tail)
*/
value type_item(value f)
	{
	if (!f->L->L || !f->L->L->L || !f->L->L->L->L) return f;
	return A(A(f->R,f->L->L->L->R),f->L->L->R);
	}

/* The pair function makes a pair of two things.  It follows the rule
(pair x y p) = (p x y), and can be defined as
	\pair = (\x\y\p p x y)
*/
value type_pair(value f)
	{
	if (!f->L->L || !f->L->L->L) return f;
	return A(A(f->R,f->L->L->R),f->L->R);
	}
