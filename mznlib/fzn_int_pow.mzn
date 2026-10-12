% The constraint `int_pow` for this solver.
%
% Z3 decides `c = a^b` over the integers only for a parameter exponent, and only
% a non-negative one: the formula is a product of `b` copies of `a`. The other
% two cases are arithmetic rather than theory, so they are stated here, which is
% where `pow` is already available.
%
% The negative case is the registry's own wording, `c = 1 div pow(a, abs(b))`,
% and it recurses once into the non-negative branch. The variable case reads the
% answer off a table indexed by the exponent, as Gecode's library does.

predicate z3_int_pow(var int: a, int: b, var int: c);

predicate fzn_int_pow(var int: a, var int: b, var int: c) =
  if is_fixed(b) then
    let {
      int: y = fix(b);
    } in if y = 0 then
      c = 1
    elseif y = 1 then
      c = a
    elseif y > 0 then
      z3_int_pow(a, y, c)
    else
      c = 1 div pow(a, -y)
    endif
  else
    let {
      array [lb(b)..ub(b)] of var int: pow_table = [
        i: if i in dom(b) then pow(a, i) default lb(c) else lb(c) endif |
          i in lb(b)..ub(b),
      ];
    } in c = pow_table[b]
  endif;
