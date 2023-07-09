// (c) 2023 Philipp Klaus Krause, philipp@colecovision.eu
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option) any
// later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//
//
// Generalized constant propagation.

#undef DEBUG_GCP_ANALYSIS
#undef DEBUG_GCP_OPT

#include <map>
#include <set>
#include <queue>
#include <iostream>
#include <ios>

// Workaround for boost bug #11880
#include <boost/version.hpp>
#if BOOST_VERSION == 106000
   #include <boost/type_traits/ice.hpp>
#endif

#include <boost/graph/graphviz.hpp>

#include "SDCCtree_dec.hpp" // We just need it for the titlewriter for debug cfg dumping.

extern "C"
{
#include "common.h"
}

static bool
operator != (const valinfo &v0, const valinfo &v1)
{
  if (v0.nothing && v1.nothing)
    return (true);
  return (v0.nothing != v1.nothing || v0.anything != v1.anything ||
    v0.min != v1.min || v0.max != v1.max ||
    v0.knownbitsmask != v1.knownbitsmask);
}

struct valinfos
{
  std::map <int, struct valinfo> map;
};

struct cfg_genconstprop_node
{
  iCode *ic;
};

typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS, cfg_genconstprop_node, struct valinfos> cfg_t;

// A quick-and-dirty function to get the CFG from sdcc (a simplified version of the function from SDCCralloc.hpp).
static void
create_cfg_genconstprop (cfg_t &cfg, iCode *start_ic, ebbIndex *ebbi)
{
  iCode *ic;

  std::map<int, unsigned int> key_to_index;
  {
    int i;

    for (ic = start_ic, i = 0; ic; ic = ic->next, i++)
      {
        boost::add_vertex(cfg);
        key_to_index[ic->key] = i;
        cfg[i].ic = ic;
      }
  }

  // Get control flow graph from sdcc.
  for (ic = start_ic; ic; ic = ic->next)
    {
      if (ic->op != GOTO && ic->op != RETURN && ic->op != JUMPTABLE && ic->next)
        boost::add_edge(key_to_index[ic->key], key_to_index[ic->next->key], cfg);

      if (ic->op == GOTO)
        boost::add_edge(key_to_index[ic->key], key_to_index[eBBWithEntryLabel(ebbi, ic->label)->sch->key], cfg);
      else if (ic->op == RETURN)
        boost::add_edge(key_to_index[ic->key], key_to_index[eBBWithEntryLabel(ebbi, returnLabel)->sch->key], cfg);
      else if (ic->op == IFX)
        boost::add_edge(key_to_index[ic->key], key_to_index[eBBWithEntryLabel(ebbi, IC_TRUE(ic) ? IC_TRUE(ic) : IC_FALSE(ic))->sch->key], cfg);
      else if (ic->op == JUMPTABLE) // This can create a multigraph. We actually need those multiple edges later for correctness of the analysis.
        for (symbol *lbl = (symbol *)(setFirstItem (IC_JTLABELS (ic))); lbl; lbl = (symbol *)(setNextItem (IC_JTLABELS (ic))))
          boost::add_edge(key_to_index[ic->key], key_to_index[eBBWithEntryLabel(ebbi, lbl)->sch->key], cfg);
    }
}

struct valinfo
getTypeValinfo (sym_link *type)
{
  struct valinfo v;
  v.anything = true;
  v.nothing = false;
  v.min = v.max = 0;
  v.knownbitsmask = 0;
  v.knownbits = 0;

  if (IS_BOOLEAN (type))
    {
      v.anything = false;
      v.min = 0;
      v.max = 1;
      v.knownbitsmask = ~1ull;
      v.knownbits = 0;
    }
  else if (IS_PTR (type))
    {
      v.anything = false;
      v.min = 0;
      v.max = (1ul << (GPTRSIZE * 8)) - 1;
      if (TARGET_IS_MCS51 && !IS_GENPTR (type) ||
        TARGET_PDK_LIKE && IS_PTR (type) && (DCL_TYPE (type) == CPOINTER || DCL_TYPE (type) == POINTER))
        {
          int addrbits = GPTRSIZE * 8;
          if (TARGET_IS_MCS51)
            {
              if (DCL_TYPE (type) == POINTER)
                addrbits = 7;
              else if (DCL_TYPE (type) == IPOINTER || DCL_TYPE (type) == PPOINTER)
                addrbits = 8;
              else
                addrbits = 16;
            }
          else if (TARGET_PDK_LIKE)
            addrbits = (DCL_TYPE (type) == CPOINTER ? 10 : 6) + TARGET_IS_PDK14 * 1 + TARGET_IS_PDK15 * 2 + TARGET_IS_PDK16 * 3;
          else
            wassert (0);
          unsigned long long addrmask = ~(~0ull << addrbits);
          v.knownbitsmask |= ~addrmask;
          v.knownbits &= addrmask;
          if (TARGET_IS_MCS51)
            v.knownbits |= (unsigned long long)pointerTypeToGPByte (DCL_TYPE (type), 0, 0) << 16;
          else if (TARGET_PDK_LIKE)
            v.knownbits |= (DCL_TYPE (type) == CPOINTER ? 0x8000 : 0x0000);
          else
            wassert (0);
        }
    }
  else if (IS_INTEGRAL (type) && IS_UNSIGNED (type) && bitsForType (type) < 64)
    {
      v.anything = false;
      v.min = 0;
      v.max = 0xffffffffffffffffull >> (64 - bitsForType (type));
      v.knownbitsmask = ~(0xffffffffffffffffull >> (64 - bitsForType (type)));
      v.knownbits = 0;
    }
  else if (IS_INTEGRAL (type) && !IS_UNSIGNED (type) && bitsForType (type) < 63)
    {
      v.anything = false;
      v.max = 0x7fffffffffffffffull >> (64 - bitsForType (type));
      v.min = -v.max - 1;
      v.knownbitsmask = ~(0xffffffffffffffffull >> (64 - bitsForType (type)));
      v.knownbits = 0;
    }
  return (v);
}

static void
valinfoCast (struct valinfo *result, sym_link *targettype, const struct valinfo &right);

struct valinfo
getOperandValinfo (const iCode *ic, const operand *op)
{
  wassert (ic);

  struct valinfo v;
  v.anything = true;
  v.nothing = false;
  v.min = v.max = 0;

  if (!op)
    return (v);

  sym_link *type = operandType (op);

  if (IS_INTEGRAL (type) && bitsForType (type) < 64 && !IS_OP_VOLATILE (op) &&// Todo: More exact check than this bits thing?
    (IS_OP_LITERAL (op) || IS_SYMOP (op) && SPEC_CONST (type) && OP_SYMBOL_CONST (op)->ival && IS_AST_VALUE (list2expr (OP_SYMBOL_CONST (op)->ival))))
    {
      struct valinfo v2;
      long long litval;
      if (IS_OP_LITERAL (op))
        litval = operandLitValueUll (op);
      else
        litval = ullFromVal (list2expr (OP_SYMBOL_CONST (op)->ival)->opval.val);
      v2.anything = false;
      v2.nothing = false;
      v2.min = litval;
      v2.max = litval;
      v2.knownbitsmask = ~0ull;
      v2.knownbits = litval;
      valinfoCast (&v, type, v2); // Need to cast: ival could be out of range of type.
      return (v);
    }
  else if (IS_ITEMP (op) && !IS_OP_VOLATILE (op))
    {
      if (ic->valinfos && ic->valinfos->map.find (op->key) != ic->valinfos->map.end ())
        return (ic->valinfos->map[op->key]);
      v.nothing = true;
      v.anything = false;
      return (v);
    }
  else
    return (getTypeValinfo (type));
}

static bool
valinfo_union (struct valinfo *v0, const struct valinfo &v1)
{
  bool change = false;
  auto new_anything = v0->anything || v1.anything;
  change |= (v0->anything != new_anything);
  v0->anything = new_anything;
  auto new_nothing = v0->nothing && v1.nothing;
  change |= (v0->nothing != new_nothing);
  v0->nothing = new_nothing;
  auto new_min = std::min (v0->min, v1.min);
  change |= (v0->min != new_min);
  v0->min = new_min;
  auto new_max = std::max (v0->max, v1.max);
  change |= (v0->max != new_max);
  v0->max = new_max;
  auto new_knownbitsmask = v0->knownbitsmask & v1.knownbitsmask & ~(v0->knownbits ^ v1.knownbits);
  change |= (v0->knownbitsmask != new_knownbitsmask);
  v0->knownbitsmask = new_knownbitsmask;
  return (change);
}

bool
valinfos_union (iCode *ic, int key, const struct valinfo &v)
{
  if (!ic /*|| !bitVectBitValue(ic->rlive, key)*/) // Unfortunately, rlive info is inaccurate, so we can't rely on it.
    return (false);

  if (ic->valinfos && ic->valinfos->map.find (key) != ic->valinfos->map.end())
    return (valinfo_union (&ic->valinfos->map[key], v));
  else
    {
      if (!ic->valinfos)
        ic->valinfos = new struct valinfos;
      ic->valinfos->map[key] = v;
      return (true);
    }
}

bool
valinfos_unions (iCode *ic, const struct valinfos &v)
{
  bool change = false;
  for (auto i = v.map.begin(); i != v.map.end(); ++i)
    change |= valinfos_union (ic, i->first, i->second);
  return (change);
}

static void
dump_op_info (std::ostream &os, const iCode *ic, operand *op)
{
  struct valinfo v = getOperandValinfo (ic, op);
  os << "";
  if (v.nothing)
    os << "X";
  if (v.anything)
    os << "*";
  else
    os << "[" << v.min << ", " << v.max << "] " << v.knownbitsmask;
}

// Dump cfg.
static void
dump_cfg_genconstprop (const cfg_t &cfg, std::string suffix)
{
  std::ofstream dump_file ((std::string (dstFileName) + ".dumpgenconstpropcfg" + suffix + (currFunc ? currFunc->rname : "__global") + ".dot").c_str());

  std::string *name = new std::string[num_vertices (cfg)];
  for (unsigned int i = 0; i < boost::num_vertices (cfg); i++)
    {
      std::ostringstream os;
      iCode *ic = cfg[i].ic;
      os << i << ", " << ic->key << ": (";
      os << std::showbase << std::hex;
      if (ic->left)
        dump_op_info (os, ic, IC_LEFT (ic));
      os << ", ";
      if (ic->right)
        dump_op_info (os, ic, IC_RIGHT (ic));
      os << ")";
      if (ic->resultvalinfo)
        {
          os << " -> ";
          if (ic->resultvalinfo->nothing)
            os << "X";
          else if (ic->resultvalinfo->anything)
            os << "*";
          else
            os << "[" << ic->resultvalinfo->min << ", " << ic->resultvalinfo->max << "] " << ic->resultvalinfo->knownbitsmask;
        }
      name[i] = os.str();
    }
  boost::write_graphviz(dump_file, cfg, boost::make_label_writer(name), boost::default_writer(), cfg_titlewriter((currFunc ? currFunc->rname : "__global"), " generalized constant propagation"));

  delete[] name;
}

// Update fields of valinfo struct from each other. TODO: Make some of this work also for negative v->min.
static void
valinfoUpdate (struct valinfo *v)
{
  if (v->anything || v->nothing)
    return;

  // Update bits from min/max.
  if (v->min == v->max)
    {
      v->knownbitsmask = ~0ull;
      v->knownbits = v->min;
      return;
    }
  for (int i = 0; i < 62; i++)
    {
      if (v->min >= 0 && v->max < (1ll << i))
        {
          v->knownbitsmask |= (~0ull << i);
          v->knownbits &= ~(~0ull << i);
        }
    }

  // Update min/max from bits.
  if (v->min >= 0)
    {
      unsigned long long bitmax = (v->knownbitsmask & v->knownbits) | ~v->knownbitsmask;
      if (bitmax < (unsigned long long)(v->max))
        v->max = bitmax;
      unsigned long long bitmin = v->knownbitsmask & v->knownbits;
      if (bitmin < (unsigned long long)(v->min))
        v->min = bitmin;
    }
}

static void
valinfoPlus (struct valinfo *result, sym_link *resulttype, const struct valinfo &left, const struct valinfo &right)
{
  // todo: rewrite using ckd_add when we can assume host compiler has c2x support!
  if (!left.anything && !right.anything &&
    left.min > LLONG_MIN / 2 && right.min > LLONG_MIN / 2 &&
    left.max < LLONG_MAX / 2 && right.max < LLONG_MAX / 2)
    {
      result->nothing = left.nothing || right.nothing;
      auto min = left.min + right.min;
      auto max = left.max + right.max;
      if (result->anything || min >= result->min && max <= result->max)
        {
          result->anything = false;
          result->min = min;
          result->max = max;
        }
    }
  if (IS_PTR (resulttype) && !left.anything && !right.anything)
    {
      if (TARGET_IS_MCS51)
        {
          result->knownbitsmask |= (left.knownbitsmask & 0xff0000ull);
          result->knownbits = result->knownbits & ~0xff0000ull | left.knownbits & 0xff0000ull;
        }
      else if (TARGET_PDK_LIKE)
        {
          result->knownbitsmask |= (left.knownbitsmask & 0x8000ull);
          result->knownbits = result->knownbits & ~0x8000ull | left.knownbits & 0x8000ull;
        }
    }
  if (!left.anything && !right.anything &&
    left.min >= 0 && right.min >= 0)
    {
      for (int i = 0; i < 61; i++) // If there are 0 bits in the same position on both sides, carry will be absorbed and not affect the following bit.
        {
          unsigned long long mask = (0x3ull << i);
          if ((left.knownbitsmask & mask) != mask)
            continue;
          if ((right.knownbitsmask & mask) != mask)
            continue;
          unsigned long long mask1 = (0x1ull << i);
          if ((left.knownbits & mask1) || (right.knownbits & mask1))
            continue;
          unsigned long long mask2 = (0x2ull << i);
          if ((left.knownbits & mask2) && (right.knownbits & mask2))
            continue;
          result->knownbitsmask |= mask2;
          if ((left.knownbits & mask2) || (right.knownbits & mask2))
            result->knownbits |= mask2; 
          else
            result->knownbits &= ~mask2; 
        }
    }
}

static void
valinfoMinus (struct valinfo *result, sym_link *resulttype, const struct valinfo &left, const struct valinfo &right)
{
  if (IS_PTR (resulttype) && !left.anything && !right.anything)
    {
      if (TARGET_IS_MCS51)
        {
          result->knownbitsmask |= (left.knownbitsmask & 0xff0000ull);
          result->knownbits = result->knownbits & ~0xff0000ull | left.knownbits & 0xff0000ull;
        }
      else if (TARGET_PDK_LIKE)
        {
          result->knownbitsmask |= (left.knownbitsmask & 0x8000ull);
          result->knownbits = result->knownbits & ~0x8000ull | left.knownbits & 0x8000ull;
        }
    }
  // todo: rewrite using ckd_sub when we can assume host compiler has c2x support!
  if (!left.anything && !right.anything &&
    left.min > LLONG_MIN / 2 && right.min > LLONG_MIN / 2 &&
    left.max < LLONG_MAX / 2 && right.max < LLONG_MAX / 2)
    {
      result->nothing = left.nothing || right.nothing;
      auto min = left.min - right.max;
      auto max = left.max - right.min;
      if (result->anything || min >= result->min && max <= result->max)
        {
          result->anything = false;
          result->min = min;
          result->max = max;
        }
    }
}

static void
valinfoMult (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  // todo: rewrite using ckd_mul when we can assume host compiler has c2x support!
  if (!left.anything && !right.anything &&
    left.min >=0 && right.min >= 0 &&
    left.max < (1ll << 31) && right.max < (1ll << 31))
    {
      result->nothing = left.nothing || right.nothing;
      auto min = left.min * right.min;
      auto max = left.max * right.max;
      if (result->anything || min >= result->min && max <= result->max)
        {
          result->anything = false;
          result->min = min;
          result->max = max;
        }
    }
}

static void
valinfoDiv (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && left.min >= result->min && left.max <= result->max)
    {
      if (!right.anything && right.min >= 0)
        {
          result->min = std::min (left.min, 0ll);
          result->max = std::max (left.max, 0ll);
        }
    }
  if (!right.anything && right.min > 0 && result->max >= 0)
    result->max /= right.min;
}

static void
valinfoMod (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && left.min >= result->min && left.max <= result->max)
    {
      result->min = std::min (left.min, 0ll);
      result->max = std::max (left.max, 0ll);
    }
  if (!left.anything && !right.anything && left.min >= 0 && right.min >= 0 && right.max <= result->max);
    result->max = right.max - 1;
}

static void
valinfoOr (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && !right.anything &&
    left.min >= 0 && right.min >= 0 && !result->anything)
    {
      result->nothing = left.nothing || right.nothing;
      result->min = std::max (left.min, right.min);
      result->max = std::max (left.max, right.max);
      long long max = std::min (left.max, right.max);
      for(int i = 0; max > 0; i++)
        {
          result->max |= (1ll << i);
          max >>= 1;
        } 
    }
  result->knownbitsmask = (left.knownbitsmask & right.knownbitsmask) | (left.knownbitsmask & left.knownbits) | (right.knownbitsmask & right.knownbits);
  result->knownbits = left.knownbits | right.knownbits;
}

static void
valinfoAnd (struct valinfo *result, sym_link *resulttype, const struct valinfo &left_orig, const struct valinfo &right_orig)
{
  // In iCode, bitwise and sometimes has operands of different type.
  struct valinfo left, right;
  valinfoCast (&left, resulttype, left_orig);
  valinfoCast (&right, resulttype, right_orig);

  if (!left.anything && !right.anything &&
    (left.min >= 0 || right.min >= 0))
    {
      result->anything = false;
      result->nothing = left.nothing || right.nothing;
      result->min = 0;
      if (left.min >= 0 && right.min >= 0)
        result->max = std::min (left.max, right.max);
    }
  result->knownbitsmask = (left.knownbitsmask & right.knownbitsmask) | (left.knownbitsmask & ~left.knownbits) | (right.knownbitsmask & ~right.knownbits);
  result->knownbits = left.knownbits & right.knownbits;
}

static void
valinfoXor (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && !right.anything &&
    left.min >= 0 && right.min >= 0 && !result->anything)
    {
      result->nothing = left.nothing || right.nothing;
      result->min = 0;
      result->max = std::max (left.max, right.max);
      long long max = std::min (left.max, right.max);
      for(int i = 0; max > 0; i++)
        {
          result->max |= (1ll << i);
          max >>= 1;
        } 
    }
  result->knownbitsmask = (left.knownbitsmask & right.knownbitsmask);
  result->knownbits = left.knownbits ^ right.knownbits;
}

static void
valinfoGetABit (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  result->anything = false;
  result->nothing = left.nothing || right.nothing;
  if (result->max > 0)
    {
      result->min = 0;
      result->max = 1;
    }
  result->knownbitsmask = ~1ull;
}

static void
valinfoLeft (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && !right.anything && right.min == right.max && right.max < 64)
    {
      result->nothing = left.nothing || right.nothing;
      result->knownbitsmask = (left.knownbitsmask << right.max) | ~(~0ull << right.max);
      result->knownbits = left.knownbits << right.max;
      struct valinfo rv;
      rv.nothing = result->nothing;
      rv.anything = false;
      rv.min = left.min;
      rv.max = left.max;
      for(long long r = right.max; r; r--)
        {
          if (rv.min < (-1ll << 61) || rv.max > (1ll << 61))
            return;
          rv.min <<= 1;
          rv.max <<= 1;
        }
      *result = rv;
    }
}

static void
valinfoRight (struct valinfo *result, const struct valinfo &left, const struct valinfo &right)
{
  if (!left.anything && !right.anything &&
    left.min >= 0 && right.min >= 0 && right.min <= 60)
    {
      result->anything = false;
      result->nothing = left.nothing || right.nothing;
      result->min = 0;
      auto max = (left.max >> right.min);
      if (max <= result->max)
        result->max = max;
      if (right.min == right.max)
        {
          result->knownbitsmask = left.knownbitsmask >> right.min;
          result->knownbits = left.knownbits >> right.min;
        }
    }
}

static void
valinfoCast (struct valinfo *result, sym_link *targettype, const struct valinfo &right)
{
  *result = getTypeValinfo (targettype);
  if (right.nothing)
    result->nothing = true;
  else if (!right.anything && (IS_INTEGRAL (targettype) || IS_GENPTR (targettype)) && 
    (!result->anything && right.min >= result->min && right.max <= result->max || result->anything))
    {
      result->anything = false;
      result->min = right.min;
      result->max = right.max;
      if (result->min >= 0)
        {
          result->knownbitsmask = right.knownbitsmask;
          result->knownbits = right.knownbits;
        }
      else if (result->max < 0 && bitsForType (targettype) < 64)
        {
          unsigned long long topmask = (~0ull << bitsForType (targettype));
          result->knownbitsmask = right.knownbitsmask | topmask;
          result->knownbits = right.knownbits | topmask;
        }
    }
  else if (!right.anything && IS_INTEGRAL (targettype) && SPEC_USIGN(targettype) && right.min == right.max)
    {
      result->anything = false;
      result->min = right.min & ~result->knownbitsmask;
      result->max = right.max & ~result->knownbitsmask;
      result->knownbitsmask = ~0ull;
      result->knownbits = result->min;
    }
}

static void
recompute_node (cfg_t &G, unsigned int i, ebbIndex *ebbi, std::pair<std::queue<unsigned int>, std::set<unsigned int> > &todo, bool externchange, bool end_it_quickly)
{
  iCode *const ic = G[i].ic;
  bool change = externchange;

  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  struct valinfo oldleftvalinfo = getOperandValinfo (ic, left);
  struct valinfo oldrightvalinfo = getOperandValinfo (ic, right);

  if (!ic->valinfos)
    ic->valinfos = new struct valinfos;

  // Gather incoming information.
  typedef /*typename*/ boost::graph_traits<cfg_t>::in_edge_iterator in_iter_t;
  in_iter_t in, in_end;
  for (boost::tie(in, in_end) = boost::in_edges(i, G); in != in_end; ++in)
    change |= valinfos_unions (ic, G[*in]);

  typedef /*typename*/ boost::graph_traits<cfg_t>::out_edge_iterator out_iter_t;
  out_iter_t out, out_end;
  boost::tie(out, out_end) = boost::out_edges(i, G);

  if (!change || out == out_end)
    return;

  symbol *resultsym;
  if (IC_RESULT (ic) && IS_SYMOP (IC_RESULT (ic)) && !POINTER_SET (ic))
    resultsym = OP_SYMBOL (IC_RESULT (ic));
  else
    resultsym = 0;
  struct valinfo resultvalinfo;

  struct valinfo leftvalinfo = getOperandValinfo (ic, left);
  struct valinfo rightvalinfo = getOperandValinfo (ic, right);

  bool localchange = externchange || leftvalinfo != oldleftvalinfo || rightvalinfo != oldrightvalinfo;

  switch (ic->op)
    {
    case IFX:
    case JUMPTABLE:
      for(; out != out_end; ++out)
        {
          G[*out] = *ic->valinfos;
          if (todo.second.find (boost::target(*out, G)) == todo.second.end())
            {
              todo.first.push (boost::target(*out, G));
              todo.second.insert (boost::target(*out, G));
            }
        }
      if (ic->op == IFX && bitVectnBitsOn (OP_DEFS (ic->left)) == 1) // Propagate some info on the condition into the branches.
        {
          iCode *cic = (iCode *)hTabItemWithKey (iCodehTab, bitVectFirstBit (OP_DEFS (ic->left)));
          struct valinfo v = getOperandValinfo (ic, cic->left);
          if (cic->op == '>' && IS_ITEMP (cic->left) && !IS_OP_VOLATILE (cic->left) && IS_INTEGRAL (operandType (cic->left)) &&
            IS_OP_LITERAL (IC_RIGHT (cic)) && !v.anything && !v.nothing && operandLitValueUll(IC_RIGHT (cic)) < +1000)
            {
              long long litval = operandLitValueUll (IC_RIGHT (cic));
              struct valinfo v_true = v;
              struct valinfo v_false = v;
              v_true.min = std::max (v.min, litval + 1);
              v_false.max = std::min (v.max, litval);
              valinfoUpdate (&v_true);
              valinfoUpdate (&v_false);
              int key_true = IC_TRUE (ic) ? eBBWithEntryLabel(ebbi, IC_TRUE(ic))->sch->key : ic->next->key;
              int key_false = IC_FALSE (ic) ? eBBWithEntryLabel(ebbi, IC_FALSE(ic))->sch->key : ic->next->key;
              boost::tie(out, out_end) = boost::out_edges(i, G);
              for(; out != out_end; ++out)
                if (G[boost::target(*out, G)].ic->key == key_true)
                  G[*out].map[cic->left->key] = v_true;
                else if (G[boost::target(*out, G)].ic->key == key_false)
                  G[*out].map[cic->left->key] = v_false;
            }
          if (cic->op == '<' && IS_ITEMP (cic->left) && !IS_OP_VOLATILE (cic->left) && IS_INTEGRAL (operandType (cic->left)) &&
            IS_OP_LITERAL (IC_RIGHT (cic)) && !v.anything && !v.nothing && operandLitValueUll(IC_RIGHT (cic)) < 0xffffffff)
            {
              long long litval = operandLitValueUll (IC_RIGHT (cic));
              struct valinfo v_true = v;
              struct valinfo v_false = v;
              v_true.max = std::min (v.max, litval - 1);
              v_false.min = std::max (v.min, litval);
              valinfoUpdate (&v_true);
              valinfoUpdate (&v_false);
              int key_true = IC_TRUE (ic) ? eBBWithEntryLabel(ebbi, IC_TRUE(ic))->sch->key : ic->next->key;
              int key_false = IC_FALSE (ic) ? eBBWithEntryLabel(ebbi, IC_FALSE(ic))->sch->key : ic->next->key;
              boost::tie(out, out_end) = boost::out_edges(i, G);
              for(; out != out_end; ++out)
                if (G[boost::target(*out, G)].ic->key == key_true)
                  G[*out].map[cic->left->key] = v_true;
                else if (G[boost::target(*out, G)].ic->key == key_false)
                  G[*out].map[cic->left->key] = v_false;
            }
        }
      break;
    default:
      G[*out] = *ic->valinfos;

      if (resultsym)
        resultvalinfo = getTypeValinfo (operandType (IC_RESULT (ic)));

#ifdef DEBUG_GCP_ANALYSIS
      if (localchange)
        std::cout << "Recompute node " << i << " ic " << ic->key << "\n";
#endif

      if (!localchange) // Input didn't change. No need to recompute result.
        resultsym = 0;
      else if (end_it_quickly) // Just use the very rough approximation from the type info only to speed up analysis.
        ;
      else if (IS_OP_VOLATILE (IC_RESULT (ic))) // No point trying to find out what we write to a volatile operand. At the next use, it could be anything, anyway.
        ;
      else if (ic->op == '!')
        {
          resultvalinfo.nothing = leftvalinfo.nothing;
          resultvalinfo.anything = false;
          resultvalinfo.min = 0;
          resultvalinfo.max = 1;
          resultvalinfo.knownbitsmask = ~1ull;
          resultvalinfo.knownbits = 0ull;
          if (!leftvalinfo.anything && (leftvalinfo.min > 0 || leftvalinfo.max < 0))
            resultvalinfo.max = 0;
          else if (!leftvalinfo.anything && leftvalinfo.min == 0 && leftvalinfo.max == 0)
            resultvalinfo.min = 1;
        }
      else if (ic-> op == UNARYMINUS)
        {
          struct valinfo z;
          z.nothing = false;
          z.anything = false;
          z.min = z.max = 0;
          valinfoMinus (&resultvalinfo, operandType (ic->result), z, leftvalinfo);
        }
      else if (ic->op == '<' || ic->op == GE_OP)
        {
          resultvalinfo.nothing = leftvalinfo.nothing || rightvalinfo.nothing;
          resultvalinfo.anything = false;
          resultvalinfo.min = 0;
          resultvalinfo.max = 1;
          resultvalinfo.knownbitsmask = ~1ull;
          resultvalinfo.knownbits = 0ull;
          if (!leftvalinfo.anything && !rightvalinfo.anything)
            {
              if (leftvalinfo.max < rightvalinfo.min)
                resultvalinfo.min = resultvalinfo.max = (ic->op == '<');
              else if (leftvalinfo.min >= rightvalinfo.max)
                resultvalinfo.min = resultvalinfo.max = (ic->op == GE_OP);
            }
        }
      else if (ic->op == '>' || ic->op == LE_OP)
        {
          resultvalinfo.nothing = leftvalinfo.nothing || rightvalinfo.nothing;
          resultvalinfo.anything = false;
          resultvalinfo.min = 0;
          resultvalinfo.max = 1;
          resultvalinfo.knownbitsmask = ~1ull;
          resultvalinfo.knownbits = 0ull;
          if (!leftvalinfo.anything && !rightvalinfo.anything)
            {
              if (leftvalinfo.min > rightvalinfo.max)
                resultvalinfo.min = resultvalinfo.max = (ic->op == '>');
              else if (leftvalinfo.max <= rightvalinfo.min)
                resultvalinfo.min = resultvalinfo.max = (ic->op == LE_OP);
            }
        }
      else if (ic->op == NE_OP || ic->op == EQ_OP)
        {
          resultvalinfo.nothing = leftvalinfo.nothing || rightvalinfo.nothing;
          resultvalinfo.anything = false;
          resultvalinfo.min = 0;
          resultvalinfo.max = 1;
          resultvalinfo.knownbitsmask = ~1ull;
          resultvalinfo.knownbits = 0ull;
          if (!leftvalinfo.anything && !rightvalinfo.anything && leftvalinfo.min == leftvalinfo.max && rightvalinfo.min == rightvalinfo.max)
            {
              bool one = (leftvalinfo.min == rightvalinfo.min) ^ (ic->op == NE_OP);
              resultvalinfo.min = one;
              resultvalinfo.max = one;
            }
        }
      else if (ic->op == '+')
        valinfoPlus (&resultvalinfo, operandType (ic->result), leftvalinfo, rightvalinfo);
      else if (ic->op == '-')
        valinfoMinus (&resultvalinfo, operandType (ic->result), leftvalinfo, rightvalinfo);
      else if (ic->op == '*')
        valinfoMult (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == '/')
        valinfoDiv (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == '%')
        valinfoMod (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == '|')
        valinfoOr (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == BITWISEAND)
        valinfoAnd (&resultvalinfo, operandType (ic->result), leftvalinfo, rightvalinfo);
      else if (ic->op == '^')
        valinfoXor (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == GETABIT)
        valinfoGetABit (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == LEFT_OP)
        valinfoLeft (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == RIGHT_OP)
        valinfoRight (&resultvalinfo, leftvalinfo, rightvalinfo);
      else if (ic->op == '=' && !POINTER_SET (ic) || ic->op == CAST)
        //resultvalinfo = rightvalinfo; // Doesn't work for = - sometimes = with mismatched types arrive here.
        valinfoCast (&resultvalinfo, operandType (IC_RESULT (ic)), rightvalinfo);
      valinfoUpdate (&resultvalinfo);

      if (resultsym)
        {
          if (!ic->resultvalinfo)
            ic->resultvalinfo = new struct valinfo;
          *ic->resultvalinfo = resultvalinfo;
          G[*out].map[resultsym->key] = resultvalinfo;
        }
      if (todo.second.find (boost::target(*out, G)) == todo.second.end())
        {
          todo.first.push (boost::target(*out, G));
          todo.second.insert (boost::target(*out, G));
        }
    }
}

// Calculate valinfos for all iCodes in function.
void
recomputeValinfos (iCode *sic, ebbIndex *ebbi, const char *suffix)
{
#ifdef DEBUG_ANALYSIS
  std::cout << "recomputeValinfos at " << (currFunc ? currFunc->name : "[NOFUNC]") << "\n";
#endif

  unsigned int max_rounds = 1000; // Rapidly end analysis once this number of rounds has been exceeded.

  cfg_t G;

  create_cfg_genconstprop(G, sic, ebbi);

  std::pair <std::queue<unsigned int>, std::set<unsigned int> > todo; // Nodes where valinfos need to be updated. We need a pair of a queue and a set to implement a queue with uniqe entries. A plain set wouldn't work, as we'd be working on some nodes all the time while never getting to others before we reach the round limit.

  // Process each node at least once.
  for (unsigned int i = 0; i < boost::num_vertices (G); i++)
    {
      delete G[i].ic->valinfos;
      G[i].ic->valinfos = NULL;
      delete G[i].ic->resultvalinfo;
      G[i].ic->resultvalinfo = NULL;
      recompute_node (G, i, ebbi, todo, true, false);
    }

  // Forward pass to get first approximation.
  for (unsigned int round = 0; !todo.first.empty (); round++)
    {
      // Take next node that needs updating.
      unsigned int i = todo.first.front ();
      todo.first.pop ();
      todo.second.erase (i);

#ifdef DEBUG_GCP_ANALYSIS
      std::cout << "Round " << round << " node " << i << " ic " << G[i].ic->key << "\n";
#endif
      recompute_node (G, i, ebbi, todo, false, round >= max_rounds);
    }

  // Refinement backward pass.
  // TODO

  if(options.dump_graphs)
    dump_cfg_genconstprop(G, suffix);
}

// Try to replace operands by constants
static void
optimizeValinfoConst (iCode *sic)
{
#ifdef DEBUG_GCP_OPT
  std::cout << "optimizeValinfoConst at " << (currFunc ? currFunc->name : "[NOFUNC]") << "\n";
#endif
  for (iCode *ic = sic; ic; ic = ic->next)
    {
      if (SKIP_IC2 (ic))
        ;
      else
        {
          operand *left = IC_LEFT (ic);
          operand *right = IC_RIGHT (ic);
          operand *result = IC_RESULT (ic);
          const valinfo vleft = getOperandValinfo (ic, left);
          const valinfo vright = getOperandValinfo (ic, right);
          if (ic->resultvalinfo && !ic->resultvalinfo->anything && ic->resultvalinfo->min == ic->resultvalinfo->max &&
            !(ic->op == '=' && IS_OP_LITERAL (right)))
            {
              const valinfo &vresult = *ic->resultvalinfo;
#ifdef DEBUG_GCP_OPT
              std::cout << "Replace result at " << ic->key << ". anything " << vresult.anything << " min " << vresult.min << " max " << vresult.max << "\n";
#endif
              if (IS_SYMOP (left))
                bitVectUnSetBit (OP_USES (left), ic->key);
              if (IS_SYMOP (right))
                bitVectUnSetBit (OP_USES (right), ic->key);
              ic->op = '=';
              ic->left = NULL;
              ic->right = operandFromValue (valCastLiteral (operandType (result), vresult.min, vresult.min), false);
            }
          else
            {
              if (left && IS_ITEMP (left) && !vleft.anything && vleft.min == vleft.max)
                {
#ifdef DEBUG_GCP_OPT
                  std::cout << "Replace left (" << OP_SYMBOL(left)->name << "), key " << left->key << " at " << ic->key << "\n";std::cout << "anything " << vleft.anything << " min " << vleft.min << " max " << vleft.max << "\n";
#endif
                  bitVectUnSetBit (OP_USES (left), ic->key);
                  ic->left = operandFromValue (valCastLiteral (operandType (left), vleft.min, vleft.min), false);
                }
              if (right && IS_ITEMP (right) && !vright.anything && vright.min == vright.max)
                {
#ifdef DEBUG_GCP_OPT
                  std::cout << "Replace right at " << ic->key << "\n";std::cout << "anything " << vleft.anything << " min " << vleft.min << " max " << vleft.max << "\n";
#endif
                  bitVectUnSetBit (OP_USES (right), ic->key);
                  ic->right = operandFromValue (valCastLiteral (operandType (right), vright.min, vright.min), false);
                }
            }
          
        }
    }
}

static void
optimizeNarrowOpCandidate (struct valinfo *v, operand *op, const iCode *ic)
{
  wassert (v && op);
  if (!IS_INTEGRAL (operandType (op)) && !IS_GENPTR (operandType (op)))
    v->anything = true;
  else if (IS_OP_LITERAL (op))
    {
      long long litval = operandLitValueUll (op);
      v->anything = litval < -100000 || litval > 100000;
      v->min = litval;
      v->max = litval;
    }
  else
    {
      bitVect *defs = bitVectCopy (OP_DEFS (op));
      for (int key = bitVectFirstBit (defs); bitVectnBitsOn (defs); bitVectUnSetBit (defs, key), key = bitVectFirstBit (defs))
        {
          iCode *dic = (iCode *)hTabItemWithKey (iCodehTab, key);
          wassert (dic && dic->resultvalinfo);
          if (dic->op != CAST && !(dic->op == '=' && !POINTER_SET (dic)) && // Def ok: we could just change to suitable cast.
            dic->op != ADDRESS_OF &&
            !bitVectBitValue (OP_USES (op), dic->key)) // Def ok: it is a use, and would be narrowed, too.
            {
              v->anything = true;
              return;
            }
          valinfo_union (v, *dic->resultvalinfo);
        }
      freeBitVect (defs);
      bitVect *uses = bitVectCopy (OP_USES (op));
      for (int key = bitVectFirstBit (uses); bitVectnBitsOn (uses); bitVectUnSetBit (uses, key), key = bitVectFirstBit (uses))
        {
          if (key == ic->key)
            continue;
          iCode *uic = (iCode *)hTabItemWithKey (iCodehTab, key);
          if (!uic)
            bitVectUnSetBit (OP_USES (op), key); // Looks like some earlier optimization didn't clean up properly. Do it now.
          else if (uic->op == CAST)
            valinfo_union (v, getOperandValinfo (uic, uic->right));
          else if ((uic->op == EQ_OP || uic->op == NE_OP || uic->op == '<' || uic->op == LE_OP || uic->op == '>' || uic->op == GE_OP) &&
            isOperandEqual (uic->left, ic->result))
            valinfo_union (v, getOperandValinfo (uic, uic->right));
          else if ((uic->op == LEFT_OP || uic->op == RIGHT_OP || uic->op == ROT) && !isOperandEqual (uic->left, op))
            ;
          else if (uic->op == '!' || uic->op == IFX)
            ;
          else if (uic->op == GET_VALUE_AT_ADDRESS && !isOperandEqual (IS_INTEGRAL (operandType (op)) ? uic->left : uic->right, op))
            ;
          else 
            {
              v->anything = true;
              return;
            }
        }
      freeBitVect (uses);
    }
}

static void
optimizeNarrowResultCandidate (struct valinfo *v, operand *op, const iCode *ic)
{
  wassert (v && op);
  if (!IS_INTEGRAL (operandType (op)) && !IS_GENPTR (operandType (op)))
    v->anything = true;
  else
    {
      bitVect *defs = bitVectCopy (OP_DEFS (op));
      for (int key = bitVectFirstBit (defs); bitVectnBitsOn (defs); bitVectUnSetBit (defs, key), key = bitVectFirstBit (defs))
        {
          if (key == ic->key)
            continue;
          iCode *dic = (iCode *)hTabItemWithKey (iCodehTab, key);
          if (!dic)
            bitVectUnSetBit (OP_USES (op), key); // Looks like some earlier optimization didn't clean up properly. Do it now.
          else if ((dic->op == CAST || dic->op == '=' && !POINTER_SET (dic)) && dic->resultvalinfo)
            valinfo_union (v, *(dic->resultvalinfo));
          else
            {
              v->anything = true;
              return;
            }
        }
      freeBitVect (defs);
      bitVect *uses = bitVectCopy (OP_USES (op));
      for (int key = bitVectFirstBit (uses); bitVectnBitsOn (uses); bitVectUnSetBit (uses, key), key = bitVectFirstBit (uses))
        {
          iCode *uic = (iCode *)hTabItemWithKey (iCodehTab, key);
          if (!uic)
            bitVectUnSetBit (OP_USES (op), key); // Looks like some earlier optimization didn't clean up properly. Do it now.
          else if (uic->op == '!' || uic->op == CAST || uic->op == IFX)
            ;
          else if (uic->op == EQ_OP || uic->op == NE_OP ||
            uic->op == '<' || uic->op == LE_OP || uic->op == '>' || uic->op == GE_OP)
            {
              const operand *otherop = isOperandEqual (op, uic->left) ? uic->right: uic->left;
              valinfo_union (v, getOperandValinfo (uic, otherop));
            }
          else if ((uic->op == LEFT_OP || uic->op == RIGHT_OP || uic->op == ROT) && !isOperandEqual (uic->left, op))
            ;
          else if (bitVectBitValue (OP_DEFS (op), uic->key)) // Ok: The use is the definition, that would get narrowed.
            ;
          else if (uic->op == GET_VALUE_AT_ADDRESS && !isOperandEqual (IS_INTEGRAL (operandType (op)) ? uic->left : uic->right, op))
            ;
          else
            {
              v->anything = true;
              break;
            }
        }
      freeBitVect (uses);
    }
}

static void
reTypeOp (operand *op, sym_link *newtype)
{
  if (IS_OP_LITERAL (op))
    {
      op->svt.valOperand = valCastLiteral (newtype, operandLitValue (op), operandLitValueUll (op));
      return;
    }

  // Replace at uses.
  bitVect *uses = bitVectCopy (OP_USES (op));
  for (int key = bitVectFirstBit (uses); bitVectnBitsOn (uses); bitVectUnSetBit (uses, key), key = bitVectFirstBit (uses))
    {
      iCode *uic = (iCode *)hTabItemWithKey (iCodehTab, key);
      wassert (uic);
      if (isOperandEqual (op, uic->left))
        setOperandType (uic->left, newtype);
      if (isOperandEqual (op, uic->right))
        setOperandType (uic->right, newtype);
    }
  freeBitVect (uses);

  // Replace at definitions.
  bitVect *defs = bitVectCopy (OP_DEFS (op));
  for (int key = bitVectFirstBit (defs); bitVectnBitsOn (defs); bitVectUnSetBit (defs, key), key = bitVectFirstBit (defs))
    {
      iCode *dic = (iCode *)hTabItemWithKey (iCodehTab, key);
      wassert (dic && dic->result && isOperandEqual (op, dic->result));
      setOperandType (dic->result, newtype);
      if (dic->op == '=' && IS_OP_LITERAL (dic->right))
        reTypeOp (dic->right, newtype);
      else if (dic->op == CAST || dic->op == '=')
        {
          dic->op = CAST;
          dic->left = operandFromLink (newtype);
        }
    }
  freeBitVect (defs);
}

// todo: Remove this, use stdc_bit_width instead once we can assume C2X support on host compiler
#ifndef ULLONG_WIDTH // Also a C2X feature
#define ULLONG_WIDTH (CHAR_BIT * sizeof (unsigned long long))
#endif
unsigned int my_stdc_bit_width (unsigned long long value)
{
  unsigned int width = 0;
  for (int i = 0; i < ULLONG_WIDTH; i++)
    if (value & (1ull << i))
      width = (i + 1);
  return width;
}

static void
optimizeBinaryOpWithoutResult (iCode *ic)
{
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);

  if (!IS_INTEGRAL (operandType (left)) || !IS_ITEMP (left) && !IS_OP_LITERAL (left) || !IS_INTEGRAL (operandType (right)) || !IS_ITEMP (right) && !IS_OP_LITERAL (right))
    return;

  struct valinfo leftv = getOperandValinfo (ic, left);
  struct valinfo rightv = getOperandValinfo (ic, right);

  optimizeNarrowOpCandidate (&leftv, left, ic);
  optimizeNarrowOpCandidate (&rightv, right, ic);

  valinfo_union (&leftv, rightv);

  if (leftv.anything || leftv.min < 0) // Todo: Relax this by also using signed _BitInt where doing so makes sense.
    return;

  unsigned int width = my_stdc_bit_width (leftv.max);
  width = ((width + 7) & (-8)); // Round up to multiple of 8.
  if (width >= bitsForType (operandType (left)) || width >= bitsForType (operandType (right)))
    return;
  if (width > port->s.bitint_maxwidth)
    return;

#ifdef DEBUG_GCP_OPT
  std::cout << "Replacing binary operation at " << ic->key << ", by cheaper one on unsigned _BitInt(" << width << ")\n";
#endif

  sym_link *newtype = newBitIntLink (width);
  SPEC_USIGN (newtype) = true;

  reTypeOp (left, newtype);
  reTypeOp (right, newtype);
}

static void
optimizeBinaryOpWithResult (iCode *ic)
{
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  operand *result = IC_RESULT (ic);

  if (!IS_INTEGRAL (operandType (result)) && !IS_PTR (operandType (result)) || !IS_ITEMP (result) || !ic->resultvalinfo)
    return;

  if (!IS_ITEMP (left) && !IS_OP_LITERAL (left) || !IS_ITEMP (right) && !IS_OP_LITERAL (right))
    return;

  struct valinfo leftv = getOperandValinfo (ic, left);
  struct valinfo rightv = getOperandValinfo (ic, right);
  struct valinfo resultv = *ic->resultvalinfo;
  
  optimizeNarrowOpCandidate (&leftv, left, ic);
  valinfo_union (&resultv, leftv);
  if (IS_INTEGRAL (operandType (result)))
    {
      optimizeNarrowOpCandidate (&rightv, right, ic);
      valinfo_union (&resultv, rightv);
    }
  optimizeNarrowResultCandidate (&resultv, result, ic);

  if (resultv.anything || resultv.min < 0) // Todo: Relax this by also using signed _BitInt where doing so makes sense.
    return;

  sym_link *newtype;
  if (IS_INTEGRAL (operandType (result)))
    {
      unsigned int width = my_stdc_bit_width (resultv.max);
      width = ((width + 7) & (-8)); // Round up to multiple of 8.
      if (width >= bitsForType (operandType (result)))
        return;
      if (width > port->s.bitint_maxwidth)
        return;

#ifdef DEBUG_GCP_OPT
      std::cout << "Replacing binary operation at " << ic->key << ", by cheaper one on unsigned _BitInt(" << width << ")\n";
#endif
      newtype = newBitIntLink (width);
      SPEC_USIGN (newtype) = true;
    }
  else // Ptr
    {
      if (!IS_GENPTR (operandType (result)) || !TARGET_PDK_LIKE && !TARGET_IS_MCS51)
        return;

      if (TARGET_IS_MCS51 && (resultv.knownbitsmask & 0xff0000) != 0xff0000) // Check if we fully know the GPByte, and thus the intrinsic named address space this pointer points to.
        return;
      if (TARGET_PDK_LIKE && !(resultv.knownbitsmask & 0x8000)) // Check if we know the topmost bit, and thus the intrinsic named address space this pointer points to.
        return;
#ifdef DEBUG_GCP_OPT
      std::cout << "Known intrinsic named address space at " << ic->key << " for " << OP_SYMBOL (result)->name << ": " << std::hex << resultv.knownbits << "\n";
#endif
      newtype = copyLinkChain (operandType (result));

      if (TARGET_IS_MCS51)
        {
          int gpbyte = (resultv.knownbits & 0xff0000) >> 16;
          if (gpbyte == GPTYPE_NEAR)
            DCL_TYPE (newtype) = IPOINTER;
          else if (gpbyte == GPTYPE_XSTACK)
            DCL_TYPE (newtype) = PPOINTER;
          else if (gpbyte == GPTYPE_FAR)
            DCL_TYPE (newtype) = FPOINTER;
          else if (gpbyte == GPTYPE_CODE)
            DCL_TYPE (newtype) = CPOINTER;
          else
            {
              std::cerr << "Odd gpbyte " << std::hex << gpbyte << "\n";
              wassert (0);
            }
        }
      else if (TARGET_PDK_LIKE)
        {
          if (resultv.knownbits & 0x8000)
            DCL_TYPE (newtype) = CPOINTER;
          else
            DCL_TYPE (newtype) = POINTER;
        }
      else
        wassert (0);
    }

  reTypeOp (left, newtype);
  reTypeOp (result, newtype);

  if (IS_PTR (operandType (result)))
    return;
  reTypeOp (right, newtype);

  // Now also retype the other operand at some uses of the result.
  bitVect *uses = bitVectCopy (OP_USES (result));
  for (int key = bitVectFirstBit (uses); bitVectnBitsOn (uses); bitVectUnSetBit (uses, key), key = bitVectFirstBit (uses))
    {
      iCode *uic = (iCode *)hTabItemWithKey (iCodehTab, key);
      wassert (uic);
      if (uic->key == ic->key)
        ;
      else if (uic->op == '!' || uic->op == CAST || uic->op == IFX || uic->op == LEFT_OP || uic->op == RIGHT_OP || uic->op == ROT)
        ;
      else if (uic->op == EQ_OP || uic->op == NE_OP ||
        uic->op == '<' || uic->op == LE_OP || uic->op == '>' || uic->op == GE_OP)
        {
          if (!isOperandEqual (result, uic->left))
            reTypeOp (uic->left, newtype);
          if (!isOperandEqual (result, uic->right))
            reTypeOp (uic->right, newtype);
        }
      else
        wassert (0);
    }
}

static void
optimizeUnaryOpWithoutResult (iCode *ic)
{
  operand *op = (ic->op == '!' || ic->op == IFX || ic->op == GET_VALUE_AT_ADDRESS) ? ic->left : ic->right;

  if (!IS_INTEGRAL (operandType (op)) && !IS_PTR (operandType (op)) || !IS_ITEMP (op))
    return;

  struct valinfo opv = getOperandValinfo (ic, op);

  optimizeNarrowOpCandidate (&opv, op, ic);

  if (opv.anything || opv.min < 0) // Todo: Relax this by also using signed _BitInt where doing so makes sense.
    return;

  sym_link *newtype;
  if (IS_INTEGRAL (operandType (op)))
    {
      unsigned int width = my_stdc_bit_width (opv.max);
      width = ((width + 7) & (-8)); // Round up to multiple of 8.
      if (width >= bitsForType (operandType (op)))
        return;
      if (width > port->s.bitint_maxwidth)
        return;
#ifdef DEBUG_GCP_OPT
      std::cout << "Replacing unary operation at " << ic->key << ", by cheaper one on unsigned _BitInt(" << width << ")\n";
#endif
      newtype = newBitIntLink (width);
      SPEC_USIGN (newtype) = true;
    }
  else
    {
      if (!IS_GENPTR (operandType (op)) || !TARGET_PDK_LIKE && !TARGET_IS_MCS51)
        return;

      if (TARGET_IS_MCS51 && (opv.knownbitsmask & 0xff0000) != 0xff0000) // Check if we fully know the GPByte, and thus the intrinsic named address space this pointer points to.
        return;
      if (TARGET_PDK_LIKE && !(opv.knownbitsmask & 0x8000)) // Check if we know the topmost bit, and thus the intrinsic named address space this pointer points to.
        return;
#ifdef DEBUG_GCP_OPT
      std::cout << "Known intrinsic named address space at " << ic->key << " for " << OP_SYMBOL (op)->name << ": " << std::hex << opv.knownbits << "\n";
#endif
      newtype = copyLinkChain (operandType (op));

      if (TARGET_IS_MCS51)
        {
          int gpbyte = (opv.knownbits & 0xff0000) >> 16;
          if (gpbyte == GPTYPE_NEAR)
            DCL_TYPE (newtype) = IPOINTER;
          else if (gpbyte == GPTYPE_XSTACK)
            DCL_TYPE (newtype) = PPOINTER;
          else if (gpbyte == GPTYPE_FAR)
            DCL_TYPE (newtype) = FPOINTER;
          else if (gpbyte == GPTYPE_CODE)
            DCL_TYPE (newtype) = CPOINTER;
          else
            {
              std::cerr << "Odd gpbyte " << std::hex << gpbyte << "\n";
              wassert (0);
            }
        }
      else if (TARGET_PDK_LIKE)
        {
          if (opv.knownbits & 0x8000)
            DCL_TYPE (newtype) = CPOINTER;
          else
            DCL_TYPE (newtype) = POINTER;
        }
      else
        wassert (0);
    }

  reTypeOp (op, newtype);
}

static void
optimizeUnaryOpWithResult (iCode *ic)
{
  operand *left =ic->left;
  operand *result = ic->result;

  if (!IS_INTEGRAL (operandType (result)) || !IS_ITEMP (result) || !ic->resultvalinfo)
    return;

  if (!IS_ITEMP (left))
    return;

  struct valinfo leftv = getOperandValinfo (ic, left);
  struct valinfo resultv = *ic->resultvalinfo;
  
  optimizeNarrowOpCandidate (&leftv, left, ic);
  optimizeNarrowResultCandidate (&resultv, result, ic);

  valinfo_union (&resultv, leftv);

  if (resultv.anything || resultv.min < 0) // Todo: Relax this by also using signed _BitInt where doing so makes sense.
    return;

  unsigned int width = my_stdc_bit_width (resultv.max);
  width = ((width + 7) & (-8)); // Round up to multiple of 8.
  if (width >= bitsForType (operandType (result)))
    return;
  if (width > port->s.bitint_maxwidth)
    return;

#ifdef DEBUG_GCP_OPT
  std::cout << "Replacing unary operation at " << ic->key << ", by cheaper one on unsigned _BitInt(" << width << ")\n";
#endif

  sym_link *newtype = newBitIntLink (width);
  SPEC_USIGN (newtype) = true;

  reTypeOp (left, newtype);
  reTypeOp (result, newtype);

  // Now also retype the other operand at some uses of the result.
  bitVect *uses = bitVectCopy (OP_USES (result));
  for (int key = bitVectFirstBit (uses); bitVectnBitsOn (uses); bitVectUnSetBit (uses, key), key = bitVectFirstBit (uses))
    {
      iCode *uic = (iCode *)hTabItemWithKey (iCodehTab, key);
      wassert (uic);
      if (uic->key == ic->key)
        ;
      else if (uic->op == '!' || uic->op == CAST || uic->op == IFX || uic->op == LEFT_OP || uic->op == RIGHT_OP || uic->op == ROT)
        ;
      else if (uic->op == EQ_OP || uic->op == NE_OP ||
        uic->op == '<' || uic->op == LE_OP || uic->op == '>' || uic->op == GE_OP)
        {
          if (!isOperandEqual (result, uic->left))
            reTypeOp (uic->left, newtype);
          if (!isOperandEqual (result, uic->right))
            reTypeOp (uic->right, newtype);
        }
      else
        wassert (0);
    }
}

static void
optimizeMult (iCode *ic)
{
  operand *left = IC_LEFT (ic);
  operand *right = IC_RIGHT (ic);
  operand *result = IC_RESULT (ic);

  sym_link *oldoptype = operandType (left);
  sym_link *oldresulttype = operandType (result);
  
  if (!IS_INTEGRAL (oldresulttype) || bitsForType (oldresulttype) <= 16 || bitsForType (oldoptype) <= 8)
    return;

  struct valinfo leftv = getOperandValinfo (ic, left);
  struct valinfo rightv = getOperandValinfo (ic, right);
  struct valinfo resultv = *ic->resultvalinfo;

  if (leftv.anything || rightv.anything || leftv.min < 0 || rightv.min < 0 || leftv.max > 0xffff || rightv.max > 0xffff || resultv.max > 0xffff)
    return;

  sym_link *newoptype;
  sym_link *newresulttype;
  if (leftv.max <= 0xff && rightv.max <= 0xff) // Use unsigned char, as that allows 8x8->16 multiplication.
    {
      newoptype = newCharLink ();
      SPEC_USIGN (newoptype) = true;
      newresulttype = (resultv.max <= 0xff) ? newCharLink () : newIntLink ();
      SPEC_USIGN (newresulttype) = true;
    }
  else
    {
      newoptype = newBitIntLink (16);
      SPEC_USIGN (newoptype) = true;
      newresulttype = newBitIntLink (16);
      SPEC_USIGN (newresulttype) = true;
    }
  
  prependCast (ic, left, newoptype, 0);
  prependCast (ic, right, newoptype, 0);
  appendCast (ic, newresulttype, 0);
}

// Try to narrow operations
static void
optimizeValinfoNarrow (iCode *sic)
{
#ifdef DEBUG_GCP_OPT
  std::cout << "optimizeValifoNarrow at " << (currFunc ? currFunc->name : "[NOFUNC]") << "\n";
#endif
  for (iCode *ic = sic; ic; ic = ic->next)
    {
      if (ic->op == '<' || ic->op == '>' || ic->op == LE_OP || ic->op == GE_OP || ic->op == EQ_OP || ic->op == NE_OP)
        optimizeBinaryOpWithoutResult (ic);
      else if (ic->op == '+' || ic->op == '-' || ic->op == '^' || ic->op == '|' || ic->op == BITWISEAND)
        optimizeBinaryOpWithResult (ic);
      else if (ic->op == '!' || ic->op == CAST || ic->op == IFX || ic->op == LEFT_OP || ic->op == RIGHT_OP || ic->op == ROT || ic->op == GET_VALUE_AT_ADDRESS)
        optimizeUnaryOpWithoutResult (ic);
      else if (ic->op == '~' || ic->op == UNARYMINUS || ic->op == LEFT_OP || ic->op == RIGHT_OP) // Not ROT, since the width of the left operand matters for the semantics.
        optimizeUnaryOpWithResult (ic);
    }

  for (iCode *ic = sic; ic; ic = ic->next)
    if (ic->op == '*')
      optimizeMult (ic);
}

// Do machine-independent optimizations based on valinfos.
void
optimizeValinfo (iCode *sic)
{
  optimizeValinfoConst (sic);
  optimizeValinfoNarrow (sic);
}

