#include <ctype.h>
#include "die.h"
#include "value.h"
#include "basic.h"
#include "buf.h"
#include "long.h"
#include "memory.h"
#include "parse.h"
#include "run.h"
#include "string.h"
#include "sym.h"

static long line;     /* current line number */
static char *error;   /* syntax error if any */

static value C;
static value I;
static value Y;
static value query;

static value parse_exp(void);
static value lambda(value, value);

/*
Grammar:

exp  =>  empty
exp  =>  term exp
exp  =>  ; exp
exp  =>  \ sym def exp

term =>  sym
term =>  ( exp )

def  =>  empty
def  =>  is term

is   =>  =
is   =>  ==

sym  => name
sym  => string

string => simple_string
string => complex_string

The Fexl parser reads an expression from the input until it reaches -1 (end of
file) or the special token "\\".  The \\ token stops the parser immediately, as
if it had reached end of file.
*/

/* The read_ch function reads the next character.  It can be set to read from
any source such as a file or a string. */
int (*read_ch)(void);

static int ch = 0;      /* current character */

static void next_ch(void)
	{
	ch = read_ch();
	if (ch == '\n') line++;
	}

static void skip_line(void)
	{
	while (1)
		{
		if (ch == '\n' || ch == -1) break;
		next_ch();
		}
	next_ch();
	}

static int at_white(void)
	{
	return isspace(ch) || ch == 0;
	}

static void skip_white(void)
	{
	while (1)
		{
		if (!at_white() || ch == -1) break;
		next_ch();
		}
	}

static void skip_filler(void)
	{
	while (1)
		{
		if (ch == '#')
			skip_line();
		else if (at_white())
			skip_white();
		else
			break;
		}
	}

static value collect_string(char *term_string, long term_len, long first_line)
	{
	struct buf *buf = 0;
	long match_pos = 0;

	while (1)
		{
		if (ch == term_string[match_pos])
			{
			match_pos++;
			next_ch();
			}
		else if (match_pos > 0)
			{
			if (match_pos >= term_len)
				break;

			long i;
			for (i = 0; i < match_pos; i++)
				buf_add(&buf, term_string[i]);

			match_pos = 0;
			}
		else if (ch == -1)
			{
			line = first_line;
			error = "Unclosed string";
			break;
			}
		else
			{
			buf_add(&buf, ch);
			next_ch();
			}
		}

	if (error)
		{
		buf_discard(&buf);
		return 0;
		}

	long len;
	char *string = buf_clear(&buf,&len);
	return Qchars(string,len);
	}

static value parse_simple_string(void)
	{
	long first_line = line;
	next_ch();
	return collect_string("\"", 1, first_line);
	}

static value parse_complex_string(void)
	{
	long first_line = line;
	struct buf *term = 0;

	while (1)
		{
		if (at_white() || ch == -1) break;
		buf_add(&term, ch);
		next_ch();
		}

	if (ch == -1)
		{
		line = first_line;
		error = "Unclosed string terminator";
		}

	if (error)
		{
		buf_discard(&term);
		return 0;
		}

	long term_len;
	char *term_string = buf_clear(&term,&term_len);
	next_ch();

	value str = collect_string(term_string, term_len, first_line);
	free_memory(term_string, term_len+1);
	return str;
	}

/*
A name may contain just about anything, except for white space (including NUL)
and a few other special characters.  This is the simplest possible rule that
can work.
*/
static value parse_name(int allow_eq)
	{
	struct buf *buf = 0;
	while (1)
		{
		if (
			at_white()
			|| ch == '\\'
			|| ch == '('
			|| ch == ')'
			|| ch == ';'
			|| ch == '"'
			|| ch == '~'
			|| ch == '#'
			|| (ch == '=' && !allow_eq)
			|| ch == -1
			)
			break;

		buf_add(&buf, ch);
		next_ch();
		}

	long len;
	char *string = buf_clear(&buf,&len);
	return Qname(string,len);
	}

/* Parse a symbol. */
static value parse_sym(int allow_eq)
	{
	if (ch == '"')
		return parse_simple_string();
	else if (ch == '~')
		return parse_complex_string();
	else
		return parse_name(allow_eq);
	}

static value find_sym(value list, value name)
	{
	value pos = list;
	while (1)
		{
		if (pos == 0) return 0;
		if (sym_eq(name,pos->L)) return pos->L;
		pos = pos->R;
		}
	}

/* Symbols defined with lambda forms inside the source text */
static value inner_syms = 0;
/* Symbols defined outside the source text */
static value outer_syms = 0;
/* Line numbers of outer symbols */
static value outer_places = 0;

/* Set a value reference to a new value. */
static void set(value *pos, value val)
	{
	hold(val);
	value old = *pos;
	if (old) drop(old);
	*pos = val;
	}

/* Push a value on the stack. */
static void push(value *stack, value f)
	{
	set(stack, A(f,*stack));
	}

/* Pop a value from the stack. */
static void pop(value *stack)
	{
	set(stack, (*stack)->R);
	}

/* Return the originally encountered occurrence of a symbol.  This unifies
multiple occurrences into a single one. */
static value orig_sym(value sym, long first_line)
	{
	hold(sym);
	value orig = find_sym(inner_syms,sym);

	if (!orig)
		{
		orig = find_sym(outer_syms,sym);

		if (!orig)
			{
			/* new outer sym encountered */
			orig = sym;
			push(&outer_syms, orig);
			push(&outer_places, Qlong(first_line));
			}
		}

	drop(sym);
	return orig;
	}

static value parse_term(void)
	{
	long first_line = line;

	value exp;

	if (ch == '(')
		{
		next_ch();
		exp = parse_exp();
		if (exp)
			{
			if (ch == ')')
				next_ch();
			else
				{
				line = first_line;
				error = "Unclosed parenthesis";
				}
			}
		}
	else
		{
		exp = parse_sym(1);
		if (exp)
			{
			if (exp->T == type_name && string_len(exp) == 0)
				{
				line = first_line;
				error = "Missing definition";
				}
			else
				exp = orig_sym(exp,first_line);
			}
		}

	if (error)
		{
		hold(exp);
		drop(exp);
		exp = 0;
		}

	return exp;
	}

static void type_open(value f) { type_error(); }

int is_open(value f)
	{
	return f->T == type_open || f->T == type_name || f->T == type_string;
	}

static value apply(value f, value x)
	{
	if (f == I) return x;

	value result = A(f,x);
	if (is_open(f) && is_open(x))
		result->T = type_open;
	return result;
	}

/*
Parse a lambda form following the initial '\' character.

An ordinary lambda symbol is terminated by white space or '=', for example:

  \x=4
  \y = 5

If you want a lambda symbol to contain '=', you must use white space after the
initial '\'.  This tells the parser that the lambda symbol is terminated by
white space only, and not '='.  For example:

  \ =   = num_eq
  \ ==  = num_eq
  \ !=  = num_ne
  \ <   = num_lt
  \ <=  = num_le
  \ >   = num_gt
  \ >=  = num_ge
*/
static value parse_lambda(void)
	{
	int allow_eq = at_white();
	skip_white();

	value sym = parse_sym(allow_eq);
	if (sym == 0) return 0;

	if (sym->T == type_name && string_len(sym) == 0)
		{
		error = "Missing lambda symbol";
		hold(sym);
		drop(sym);
		return 0;
		}

	hold(sym);

	skip_filler();
	int has_def = (ch == '=');

	value val = 0;

	if (has_def)
		{
		next_ch();
		if (ch == '=')
			{
			/* Recursive definition */
			next_ch();
			skip_filler();

			push(&inner_syms,sym);
			value def = parse_term();
			hold(def);
			if (def)
				{
				value body = parse_exp();
				if (body)
					{
					val = lambda(sym,body);
					val = apply(val,apply(Y,lambda(sym,def)));
					}
				}
			drop(def);
			pop(&inner_syms);
			}
		else
			{
			/* Non-recursive definition, forces eager evaluation */
			skip_filler();

			value def = parse_term();
			hold(def);
			if (def)
				{
				push(&inner_syms,sym);
				value body = parse_exp();
				if (body)
					{
					val = lambda(sym,body);
					val = apply(apply(query,def),val);
					}
				pop(&inner_syms);
				}
			drop(def);
			}
		}
	else
		{
		/* Normal parameter (symbol without definition) */
		push(&inner_syms,sym);
		value body = parse_exp();
		if (body) val = lambda(sym,body);
		pop(&inner_syms);
		}

	drop(sym);
	return val;
	}

/*
If you're parsing a possibly malicious script, you should use setrlimit to
impose limits not only on stack depth, but also on total memory usage and
CPU time.

For example, if you parse an infinite stream of '(' characters, you'll see a
segmentation fault due to stack overflow.  That happens due to the default
limits on stack size, without any intervention.  But if you parse an infinite
stream of ' ' characters, it will run forever unless you impose limits on CPU
time with RLIMIT_CPU.  If you parse an infinite stream of 'a' characters, it
will use an unbounded amount of memory which could slow your machine to a crawl
until it finally reaches the very large default limits.  So you should set
RLIMIT_AS, and also RLIMIT_DATA for good measure, to limit the total amount of
memory.
*/

/* Parse an expression. */
static value parse_exp(void)
	{
	value exp = I;

	while (1)
		{
		skip_filler();
		if (ch == -1 || ch == ')') break;

		value val;

		if (ch == '\\')
			{
			next_ch();
			if (ch == '\\')
				{
				ch = -1; /* Two backslashes simulates end of file. */
				break;
				}

			val = parse_lambda();
			}
		else if (ch == ';')
			{
			next_ch();
			val = parse_exp();
			}
		else
			val = parse_term();

		if (val == 0)
			{
			hold(exp);
			drop(exp);
			exp = 0;
			break;
			}

		exp = apply(exp,val);
		}

	return exp;
	}

/* Return a copy of f with x substituted according to pattern p. */
static value subst(value p, value f, value x)
	{
	if (p->T == reduce_C) return f;
	if (p->T == reduce_I) return x;
	return A(subst(p->L,f->L,x),subst(p->R,f->R,x));
	}

/* lambda pattern form value = subst(pattern,form,value) */
static void reduce3_lambda(value f)
	{
	replace(f, subst(f->L->L->R,f->L->R,f->R));
	}

static void reduce2_lambda(value f)
	{
	f->T = reduce3_lambda;
	}

void type_lambda(value f)
	{
	f->T = reduce2_lambda;
	}

/* Get the pattern of occurrences of symbol x in form f. */
static value get_pattern(value x, value f)
	{
	if (f == x) return I;
	if (f->T && f->T != type_open) return C;

	value pl = get_pattern(x,f->L);
	value pr = get_pattern(x,f->R);

	if (pl->T == reduce_C && pr->T == reduce_C) return C;

	return A(pl,pr);
	}

static value lam;

/* Make a lambda form using the substitution pattern for symbol x in form f. */
static value lambda(value x, value f)
	{
	value p = get_pattern(x,f);

	if (p->T == reduce_C) return A(C,f);
	if (p->T == reduce_I) return I;

	/* This special case uses a little less time and memory. */
	if (p->L->T == reduce_C && p->R->T == reduce_I)
		{
		value result = A(I,f->L);  /* lam (C I) (f g) = f */
		hold(p); drop(p);
		hold(f); drop(f);
		return result;
		}

	return apply(A(lam,p),f);
	}

static void beg_parse(void)
	{
	C = Q(reduce_C);
	I = Q(reduce_I);
	Y = Q(reduce_Y);
	query = Q(reduce_query);
	lam = Q(type_lambda);

	hold(C);
	hold(I);
	hold(Y);
	hold(query);
	hold(lam);
	}

static void end_parse(void)
	{
	drop(C);
	drop(I);
	drop(Y);
	drop(query);
	drop(lam);
	}

/*
Parse the source text, returning (pair ok; pair exp; symbols).

ok is true if the source is well-formed, i.e. no syntax errors.

If ok is true, exp is the parsed expression.
If ok is false, exp is (pair error line), where error is the string describing
the syntax error, and line is the line number on which it occurred.

symbols is the list of all symbols used but not defined within the source text.
It is a list of entries (pair sym line_no), where line_no is the line number on
which the symbol first occurred.

If ok is true, then the caller can take the exp and successively apply the
definitions of each symbol in the symbols list.  The result will be the actual
executable function which can then be run with "eval" in the Fexl intepreter.
*/
value parse_source(void)
	{
	beg_parse();

	value exp;

	if (read_ch == 0)
		{
		error = "Can't open script";
		exp = 0;
		}
	else
		{
		line = 1;
		next_ch();

		exp = parse_exp();
		if (exp)
			{
			if (ch != -1)
				{
				error = "Extraneous input";
				hold(exp);
				drop(exp);
				exp = 0;
				}
			}
		}

	value item = Q(reduce_item);
	value pair = Q(reduce_pair);

	hold(item);
	hold(pair);

	value symbols = C;

	while (outer_syms)
		{
		value sym = outer_syms->L;
		value place = outer_places->L;

		if (exp) exp = lambda(sym,exp);
		symbols = A(A(item,A(A(pair,sym),place)),symbols);

		pop(&outer_syms);
		pop(&outer_places);
		}

	/* If there was an error, use (pair error line) as the expression. */
	if (exp == 0)
		exp = A(A(pair,Qstring(error)),Qlong(line));

	value ok = Q(error ? reduce_F : reduce_C);
	value result = A(A(pair,ok),A(A(pair,exp),symbols));

	drop(item);
	drop(pair);

	line = 0;
	error = 0;

	end_parse();
	return result;
	}
