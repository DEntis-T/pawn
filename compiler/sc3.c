/*  Pawn compiler - Recursive descend expresion parser
 *
 *  Copyright (c) CompuPhase, 1997-2023
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not
 *  use this file except in compliance with the License. You may obtain a copy
 *  of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *  License for the specific language governing permissions and limitations
 *  under the License.
 *
 *  Version: $Id: sc3.c 6965 2023-07-20 15:44:35Z thiadmer $
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>     /* for _MAX_PATH */
#include <string.h>
#if defined FORTIFY
  #include <alloc/fortify.h>
#endif
#include "sc.h"

static int skim(const int *opstr,void (*testfunc)(int),int dropval,int endval,
                int (*hier)(value*),value *lval);
static void dropout(int lvalue,void (*testfunc)(int val),int exit1,value *lval);
static int plnge(const int *opstr,int opoff,int (*hier)(value *lval),value *lval,
                 char *forcetag,int chkbitwise);
static int plnge1(int (*hier)(value *lval),value *lval);
static void plnge2(void (*oper)(void),void (*arrayoper)(cell),
                   int (*hier)(value *lval),
                   value *lval1,value *lval2);
static cell calc(cell left,const void (*oper)(),cell right,char *boolresult);
static int hier14(value *lval);
static int hier13(value *lval);
static int hier12(value *lval);
static int hier11(value *lval);
static int hier10(value *lval);
static int hier9(value *lval);
static int hier8(value *lval);
static int hier7(value *lval);
static int hier6(value *lval);
static int hier5(value *lval);
static int hier4(value *lval);
static int hier3(value *lval);
static int hier2(value *lval);
static int hier1(value *lval1);
static int primary(value *lval,int *symtok);
static void clear_value(value *lval);
static void callfunction(symbol *sym,value *lval_result,int matchparanthesis);
static int skiptotoken(int token);
static int dbltest(const void (*oper)(),value *lval1,value *lval2);
static int commutative(const void (*oper)());
static int constant(value *lval);

static char lastsymbol[sNAMEMAX+1]; /* name of last function/variable */
static int bitwise_opercount;   /* count of bitwise operators in an expression */
static int decl_heap=0;

/* Function addresses of binary operators for signed operations */
static void (* const op1[17])(void) = {
  os_mult,os_div,os_mod,        /* hier3, index 0 */
  ob_add,ob_sub,                /* hier4, index 3 */
  ob_sal,os_sar,ou_sar,         /* hier5, index 5 */
  ob_and,                       /* hier6, index 8 */
  ob_xor,                       /* hier7, index 9 */
  ob_or,                        /* hier8, index 10 */
  os_le,os_ge,os_lt,os_gt,      /* hier9, index 11 */
  ob_eq,ob_ne,                  /* hier10, index 15 */
};
static void (* const op2[17])(cell size) = {
  NULL,NULL,NULL,               /* hier3, index 0 */
  NULL,NULL,                    /* hier4, index 3 */
  NULL,NULL,NULL,               /* hier5, index 5 */
  NULL,                         /* hier6, index 8 */
  NULL,                         /* hier7, index 9 */
  NULL,                         /* hier8, index 10 */
  NULL,NULL,NULL,NULL,          /* hier9, index 11 */
  oa_eq,oa_ne,                  /* hier10, index 15 */
};
/* These two functions are defined because the functions inc() and dec() in
 * SC4.C have a different prototype than the other code generation functions.
 * The arrays for user-defined functions use the function pointers for
 * identifying what kind of operation is requested; these functions must all
 * have the same prototype. As inc() and dec() are special cases already, it
 * is simplest to add two "do-nothing" functions.
 */
static void user_inc(void) {}
static void user_dec(void) {}

#define IS_ARRAY(lval)        ( (lval).ident==iARRAY || (lval).ident==iREFARRAY )
#define IS_PSEUDO_ARRAY(lval) ( ((lval).ident==iARRAYCELL || (lval).ident==iARRAYCHAR) \
                                 && (lval).constval>1 && (lval).sym->dim.array.level==0 )


/*
 *  Searches for a binary operator a list of operators. The list is stored in
 *  the array "list". The last entry in the list should be set to 0.
 *
 *  The index of an operator in "list" (if found) is returned in "opidx". If
 *  no operator is found, nextop() returns 0.
 *
 *  If an operator is found in the expression, it cannot be used in a function
 *  call with omitted parantheses. Mark this...
 *
 *  Global references: sc_allowproccall   (modified)
 */
static int nextop(int *opidx,const int *list)
{
  *opidx=0;
  while (*list){
    if (matchtoken(*list)){
      sc_allowproccall=FALSE;
      return TRUE;      /* found! */
    } else {
      list+=1;
      *opidx+=1;
    } /* if */
  } /* while */
  return FALSE;         /* entire list scanned, nothing found */
}

/* verifies whether an operator can only be used as a binary operator (this
 * excludes the '-', which can be used in both unary and binary forms)
 */
static int isbinaryop(int tok)
{
  static const int operators[] = {'*','/','%','+',
                                  tSHL,tSHR,tSHRU,
                                  '&','^','|',
                                  tlLE,tlGE,'<','>',tlEQ,tlNE,
                                  tlAND,tlOR,
                                  '='
                                 };
  int idx;

  for (idx=0; idx<sizearray(operators) && tok!=operators[idx]; idx++)
    /* nothing */;
  return idx<sizearray(operators);
}

static const char *check_symbolname(value *lval)
{
  if (lval->sym!=NULL && strlen(lval->sym->name)>0)
    return lval->sym->name;
  return "-unknown-";
}

SC_FUNC int check_userop(void (*oper)(void),int tag1,int tag2,int numparam,
                         value *lval,int *resulttag)
{
static const char *binoperstr[] = { "*", "/", "%", "+", "-", "", "", "",
                                    "", "", "", "<=", ">=", "<", ">", "==", "!=" };
static const int binoper_savepri[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                       FALSE, FALSE, FALSE, FALSE, FALSE,
                                       TRUE, TRUE, TRUE, TRUE, FALSE, FALSE };
static const char *unoperstr[] = { "!", "-", "++", "--" };
static void (* const unopers[])(void) = { lneg, neg, user_inc, user_dec };
  char opername[4] = "";
  char symbolname[sNAMEMAX+1];
  int i,swapparams,savepri,savealt;
  int paramspassed;
  symbol *sym;

  /* since user-defined operators on untagged operands are forbidden, we have
   * a quick exit.
   */
  assert(numparam==1 || numparam==2);
  if (tag1==0 && (numparam==1 || tag2==0))
    return FALSE;

  savepri=savealt=FALSE;
  /* find the name with the operator */
  if (numparam==2) {
    if (oper==NULL) {
      /* assignment operator: a special case */
      strcpy(opername,"=");
      if (lval!=NULL && (lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR))
        savealt=TRUE;
    } else {
      assert( (sizeof binoperstr / sizeof binoperstr[0]) == (sizeof op1 / sizeof op1[0]) );
      for (i=0; i<sizeof op1 / sizeof op1[0]; i++) {
        if (oper==op1[i]) {
          strcpy(opername,binoperstr[i]);
          savepri=binoper_savepri[i];
          break;
        } /* if */
      } /* for */
    } /* if */
  } else {
    assert(oper!=NULL);
    assert(numparam==1);
    /* try a select group of unary operators */
    assert( (sizeof unoperstr / sizeof unoperstr[0]) == (sizeof unopers / sizeof unopers[0]) );
    if (opername[0]=='\0') {
      for (i=0; i<sizeof unopers / sizeof unopers[0]; i++) {
        if (oper==unopers[i]) {
          strcpy(opername,unoperstr[i]);
          break;
        } /* if */
      } /* for */
    } /* if */
  } /* if */
  /* if not found, quit */
  if (opername[0]=='\0')
    return FALSE;

  /* create a symbol name from the tags and the operator name */
  assert(numparam==1 || numparam==2);
  operator_symname(symbolname,opername,tag1,tag2,numparam,tag2);
  swapparams=FALSE;
  sym=findglb(symbolname,sGLOBAL);
  if (sym==NULL /*|| (sym->usage & uDEFINE)==0*/) {  /* should not check uDEFINE; first pass clears these bits */
    /* check for commutative operators */
    if (tag1==tag2 || oper==NULL || !commutative(oper))
      return FALSE;             /* not commutative, cannot swap operands */
    /* if arrived here, the operator is commutative and the tags are different,
     * swap tags and try again
     */
    assert(numparam==2);        /* commutative operator must be a binary operator */
    operator_symname(symbolname,opername,tag2,tag1,numparam,tag1);
    swapparams=TRUE;
    sym=findglb(symbolname,sGLOBAL);
    if (sym==NULL /*|| (sym->usage & uDEFINE)==0*/)
      return FALSE;
  } /* if */

  /* check existance and the proper declaration of this function */
  if ((sym->usage & uMISSING)!=0 || (sym->usage & uPROTOTYPED)==0) {
    char symname[2*sNAMEMAX+16];  /* allow space for user defined operators */
    funcdisplayname(symname,sym->name);
    if ((sym->usage & uMISSING)!=0)
      error(4,symname);           /* function not defined */
    if ((sym->usage & uPROTOTYPED)==0)
      error(71,symname);          /* operator must be declared before use */
  } /* if */

  /* we don't want to use the redefined operator in the function that
   * redefines the operator itself, otherwise the snippet below gives
   * an unexpected recursion:
   *    fixed:operator+(fixed:a, fixed:b)
   *        return a + b
   */
  if (sym==curfunc)
    return FALSE;

  /* for increment and decrement operators, the symbol must first be loaded
   * (and stored back afterwards)
   */
  if (oper==user_inc || oper==user_dec) {
    assert(!savepri);
    assert(lval!=NULL);
    if (lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR)
      pushreg(sPRI);            /* save current address in PRI */
    rvalue(lval);               /* get the symbol's value in PRI */
  } /* if */

  assert(!savepri || !savealt); /* either one MAY be set, but not both */
  if (savepri) {
    /* the chained comparison operators require that the ALT register is
     * unmodified, so we save it here; actually, we save PRI because the normal
     * instruction sequence (without user operator) swaps PRI and ALT
     */
    pushreg(sPRI);              /* right-hand operand is in PRI */
  } else if (savealt) {
    /* for the assignment operator, ALT may contain an address at which the
     * result must be stored; this address must be preserved accross the
     * call
     */
    assert(lval!=NULL);         /* this was checked earlier */
    assert(lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR); /* checked earlier */
    pushreg(sALT);
  } /* if */

  /* push parameters, call the function */
  paramspassed= (oper==NULL) ? 1 : numparam;
  switch (paramspassed) {
  case 1:
    pushreg(sPRI);
    break;
  case 2:
    /* note that 1) a function expects that the parameters are pushed
     * in reversed order, and 2) the left operand is in the secondary register
     * and the right operand is in the primary register */
    if (swapparams) {
      pushreg(sALT);
      pushreg(sPRI);
    } else {
      pushreg(sPRI);
      pushreg(sALT);
    } /* if */
    break;
  default:
    assert(0);
  } /* switch */
  markexpr(sPARM,NULL,0);       /* mark the end of a sub-expression */
  pushval((cell)paramspassed*pc_cellsize);
  assert(sym->ident==iFUNCTN);
  ffcall(sym,NULL,paramspassed);
  if (sc_status!=statSKIP)
    markusage(sym,uREAD);       /* do not mark as "used" when this call itself is skipped */
  if ((sym->usage & uNATIVE)!=0 && sym->x.lib!=NULL)
    sym->x.lib->value += 1;     /* increment "usage count" of the library */
  assert(resulttag!=NULL);
  *resulttag=sym->tag;          /* save tag of the called function */

  if (savepri || savealt)
    popreg(sALT);               /* restore the saved PRI/ALT into ALT */
  if (oper==user_inc || oper==user_dec) {
    assert(lval!=NULL);
    if (lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR)
      popreg(sALT);             /* restore address (in ALT) */
    store(lval);                /* store PRI in the symbol */
    swapregs();                 /* make sure PRI is restored on exit */
  } /* if */
  return TRUE;
}

SC_FUNC int matchtag(int formaltag,int actualtag,int allowcoerce)
{
  if (formaltag!=actualtag) {
    /* if the formal tag is zero and the actual tag is not "fixed", the actual
     * tag is "coerced" to zero
     */
    if (!allowcoerce || formaltag!=0 || (actualtag & FIXEDTAG)!=0)
      return FALSE;
  } /* if */
  return TRUE;
}

/*
 *  The AMX pseudo-processor has no direct support for logical (boolean)
 *  operations. These have to be done via comparing and jumping. Since we are
 *  already jumping through the code, we might as well implement an "early
 *  drop-out" evaluation (also called "short-circuit"). This conforms to
 *  standard C:
 *
 *  expr1 || expr2           expr2 will only be evaluated if expr1 is false.
 *  expr1 && expr2           expr2 will only be evaluated if expr1 is true.
 *
 *  expr1 || expr2 && expr3  expr2 will only be evaluated if expr1 is false
 *                           and expr3 will only be evaluated if expr1 is
 *                           false and expr2 is true.
 *
 *  Code generation for the last example proceeds thus:
 *
 *      evaluate expr1
 *      operator || found
 *      jump to "l1" if result of expr1 not equal to 0
 *      evaluate expr2
 *      ->  operator && found; skip to higher level in hierarchy diagram
 *          jump to "l2" if result of expr2 equal to 0
 *          evaluate expr3
 *          jump to "l2" if result of expr3 equal to 0
 *          set expression result to 1 (true)
 *          jump to "l3"
 *      l2: set expression result to 0 (false)
 *      l3:
 *      <-  drop back to previous hierarchy level
 *      jump to "l1" if result of expr2 && expr3 not equal to 0
 *      set expression result to 0 (false)
 *      jump to "l4"
 *  l1: set expression result to 1 (true)
 *  l4:
 *
 */

/*  Skim over terms adjoining || and && operators
 *  dropval   The value of the expression after "dropping out". An "or" drops
 *            out when the left hand is TRUE, so dropval must be 1 on "or"
 *            expressions.
 *  endval    The value of the expression when no expression drops out. In an
 *            "or" expression, this happens when both the left hand and the
 *            right hand are FALSE, so endval must be 0 for "or" expressions.
 */
static int skim(const int *opstr,void (*testfunc)(int),int dropval,int endval,
                int (*hier)(value*),value *lval)
{
  int hits,droplab,endlab,opidx;
  int allconst;
  cell constval;
  int index;
  cell cidx;

  stgget(&index,&cidx);         /* mark position in code generator */
  hits=FALSE;                   /* no logical operators "hit" yet */
  allconst=TRUE;                /* assume all values "const" */
  constval=0;
  droplab=0;                    /* to avoid a compiler warning */
  for ( ;; ) {
    int lvalue=plnge1(hier,lval);   /* evaluate left expression */
    int foundop;

    allconst= allconst && (lval->ident==iCONSTEXPR);
    if (allconst) {
      if (hits) {
        /* one operator was already found */
        if (testfunc==jmp_ne0)
          lval->constval= lval->constval || constval;
        else
          lval->constval= lval->constval && constval;
      } /* if */
      constval=lval->constval;  /* save result accumulated so far */
    } /* if */

    foundop=nextop(&opidx,opstr);
    if ((foundop || hits) && (lval->ident==iARRAY || lval->ident==iREFARRAY))
      error(33, check_symbolname(lval));  /* array was not indexed in an expression */
    if (foundop) {
      if (!hits) {
        /* this is the first operator in the list */
        hits=TRUE;
        droplab=getlabel();
      } /* if */
      dropout(lvalue,testfunc,droplab,lval);
    } else if (hits) {                       /* no (more) identical operators */
      dropout(lvalue,testfunc,droplab,lval); /* found at least one operator! */
      ldconst(endval,sPRI);
      jumplabel(endlab=getlabel());
      setlabel(droplab);
      ldconst(dropval,sPRI);
      setlabel(endlab);
      lval->sym=NULL;
      lval->tag=pc_addtag("bool");  /* force tag to be "bool" */
      if (allconst) {
        lval->ident=iCONSTEXPR;
        lval->constval=constval;
        stgdel(index,cidx);         /* scratch generated code and calculate */
      } else {
        lval->ident=iEXPRESSION;
        lval->constval=0;
      } /* if */
      return FALSE;
    } else {
      return lvalue;            /* none of the operators in "opstr" were found */
    } /* if */

  } /* while */
}

/*
 *  Reads into the primary register the variable pointed to by lval if
 *  plunging through the hierarchy levels detected an lvalue. Otherwise
 *  if a constant was detected, it is loaded. If there is no constant and
 *  no lvalue, the primary register must already contain the expression
 *  result.
 *
 *  After that, the compare routines "jmp_ne0" or "jmp_eq0" are called, which
 *  compare the primary register against 0, and jump to the "early drop-out"
 *  label "exit1" if the condition is true.
 */
static void dropout(int lvalue,void (*testfunc)(int val),int exit1,value *lval)
{
  if (lvalue)
    rvalue(lval);
  else if (lval->ident==iCONSTEXPR)
    ldconst(lval->constval,sPRI);
  (*testfunc)(exit1);
}

static cell checkarrays(value *lval1,value *lval2)
{
  /* If the left operand is an array, right operand should be an array variable
   * of the same size and the same dimension, an array literal (of the same
   * size) or a literal string. For single-dimensional arrays, it is permitted
   * to assign a smaller array into a larger one (without warning) and to compare
   * a large array to a smaller one. This is to make it easier to work with strings.
   * Symbolic arrays must always match exactly.
   */
  int exactmatch=TRUE;
  int level;
  int ispacked1,ispacked2;
  cell ltlength,length,totalsize1;

  assert(lval1!=NULL);
  assert(lval2!=NULL);
  assert(IS_ARRAY(*lval1) || IS_PSEUDO_ARRAY(*lval1));
  assert(lval1->sym!=NULL);
  assert(lval1->sym->name!=NULL);

  ltlength=(int)lval1->sym->dim.array.length;
  ispacked1=((lval1->sym->usage & uPACKED)!=0);
  if (IS_PSEUDO_ARRAY(*lval1)) {
    ltlength=(int)lval1->constval;
    ispacked1=lval1->ispacked;
  } /* if */
  totalsize1=array_totalsize(lval1->sym);
  if (!IS_ARRAY(*lval2) && !IS_PSEUDO_ARRAY(*lval2))
    error(33,lval1->sym->name);         /* array must be indexed */
  if (lval2->sym!=NULL) {
    if (totalsize1==0)
      return error(46,lval1->sym->name);/* unknown array size */
    if (lval2->constval==0) {
      length=lval2->sym->dim.array.length;/* array variable */
      if (lval1->sym->dim.array.names!=NULL && !IS_PSEUDO_ARRAY(*lval1)
          && (lval2->sym->dim.array.names==NULL || !compare_consttable(lval1->sym->dim.array.names,lval2->sym->dim.array.names)))
        error(47);                      /* array definitions do not match */
      ispacked2=((lval2->sym->usage & uPACKED)!=0);
    } else {
      length=lval2->constval;
      if (lval2->sym->dim.array.level!=0)
        error(28,lval2->sym->name);     /* invalid subscript (not an array) */
      ispacked2=lval2->ispacked;
    } /* if */
    level=lval2->sym->dim.array.level;
    if (level==0 && lval1->sym->dim.array.names==NULL)
      exactmatch=FALSE;
  } else {
    length=lval2->constval;             /* literal array */
    level=0;
    ispacked2=lval2->ispacked;
    if (lval1->sym->dim.array.names!=NULL)
      error(47);                        /* array definitions do not match */
    /* the destination array may have unknown size only if the source size is -1
       (for a literal string with just the terminator, e.g. string="") */
    if (totalsize1==0) {
      if (length==-1)
        ltlength=1;                     /* assume destination can hold 1 cell */
      else
        error(46,lval1->sym->name);     /* unknown array size */
    }
    /* if length is negative, it means that lval2 is a literal string; the
       destination size may be smaller than the destination array (provided
       that the destination array is not a symbolic array, checked above) */
    if (length<0) {
      length=-length;
      exactmatch=FALSE;
    }
  } /* if */
  if (lval1->sym->dim.array.level!=level)
    return error(48);           /* array dimensions must match */
  else if (ltlength<length || exactmatch && ltlength>length || length==0)
    return error(47);           /* array sizes must match */
  if (ispacked1!=ispacked2)
    error(229);                 /* mixing packed and unpacked arrays */
  if (level>0) {
    /* check the sizes of all sublevels too */
    symbol *sym1 = lval1->sym;
    symbol *sym2 = lval2->sym;
    int i;
    assert(sym1!=NULL && sym2!=NULL);
    /* ^^^ sym2 must be valid, because only variables can be
     *     multi-dimensional (there are no multi-dimensional literals),
     *     sym1 must be valid because it must be an lvalue
     */
    assert(exactmatch);
    for (i=0; i<level; i++) {
      sym1=finddepend(sym1);
      sym2=finddepend(sym2);
      assert(sym1!=NULL && sym2!=NULL);
      /* ^^^ both arrays have the same dimensions (this was checked
       *     earlier) so the dependend should always be found
       */
      if (sym1->dim.array.length!=sym2->dim.array.length)
        error(47);                      /* array sizes must match */
      if (sym1->dim.array.names!=NULL
          && (sym2->dim.array.names==NULL || !compare_consttable(sym1->dim.array.names,sym2->dim.array.names)))
        error(47);                      /* array definitions do not match */
      if ((sym1->usage & uPACKED)!=(sym2->usage & uPACKED))
        error(47);                      /* array definitions do not match */
    } /* for */
    /* get the total size in cells of the multi-dimensional array */
    length=array_totalsize(lval1->sym);
    assert(length>0);           /* already checked */
  } /* if */

  return length;
}

static void checkfunction(value *lval)
{
  symbol *sym=lval->sym;

  if (sym==NULL || (sym->ident!=iFUNCTN && sym->ident!=iREFFUNC))
    return;             /* no known symbol, or not a function result */

  if ((sym->usage & uDEFINE)!=0) {
    /* function is defined, can now check the return value (but make an
     * exception for directly recursive functions)
     */
    if (sym!=curfunc && (sym->usage & uRETVALUE)==0) {
      char symname[2*sNAMEMAX+16];  /* allow space for user defined operators */
      funcdisplayname(symname,sym->name);
      error(209,symname);       /* function should return a value */
    } /* if */
  } else {
    /* function not yet defined, set */
    sym->usage|=uRETVALUE;      /* make sure that a future implementation of
                                 * the function uses "return <value>" */
  } /* if */
}

/*
 *  Plunge to a lower level
 */
static int plnge(const int *opstr,int opoff,int (*hier)(value *lval),value *lval,
                 char *forcetag,int chkbitwise)
{
  int lvalue,opidx;
  int count;
  value lval2 = {0};

  lvalue=plnge1(hier,lval);
  if (nextop(&opidx,opstr)==0)
    return lvalue;              /* no operator in "opstr" found */
  if (lvalue)
    rvalue(lval);
  count=0;
  do {
    if (chkbitwise && count++>0 && bitwise_opercount!=0)
      error(212);
    opidx+=opoff;               /* add offset to index returned by nextop() */
    plnge2(op1[opidx],op2[opidx],hier,lval,&lval2);
    if (op1[opidx]==ob_and || op1[opidx]==ob_or)
      bitwise_opercount++;
    if (forcetag!=NULL)
      lval->tag=pc_addtag(forcetag);
  } while (nextop(&opidx,opstr)); /* do */
  return FALSE;         /* result of expression is not an lvalue */
}

/*  plnge_rel
 *
 *  Binary plunge to lower level; this is very simular to plnge, but
 *  it has special code generation sequences for chained operations.
 */
static int plnge_rel(const int *opstr,int opoff,int (*hier)(value *lval),value *lval)
{
  int lvalue,opidx;
  value lval2={0};
  int count;

  /* this function should only be called for relational operators */
  assert(op1[opoff]==os_le);
  lvalue=plnge1(hier,lval);
  if (nextop(&opidx,opstr)==0)
    return lvalue;              /* no operator in "opstr" found */
  if (lvalue)
    rvalue(lval);
  count=0;
  lval->boolresult=TRUE;
  do {
    /* same check as in plnge(), but "chkbitwise" is always TRUE */
    if (count>0 && bitwise_opercount!=0)
      error(212);       /* possibly unintended bitwise operation */
    if (count>0) {
      relop_prefix();
      /* copy right hand expression of the previous iteration, but keep the
       * constant boolresult value of the previous calculation
       */
      lval2.boolresult=lval->boolresult;
      *lval=lval2;
    } /* if */
    opidx+=opoff;
    plnge2(op1[opidx],op2[opidx],hier,lval,&lval2);
    if (count++>0)
      relop_suffix();
  } while (nextop(&opidx,opstr)); /* enddo */
  lval->constval=lval->boolresult;
  if (lval->ident!=iCONSTEXPR || lval2.ident!=iCONSTEXPR)
    lval->ident=iEXPRESSION;
  lval->tag=pc_addtag("bool");    /* force tag to be "bool" */
  return FALSE;         /* result of expression is not an lvalue */
}

/*  plnge1
 *
 *  Unary plunge to lower level
 *  Called by: skim(), plnge(), plnge2(), plnge_rel(), hier14() and hier13()
 */
static int plnge1(int (*hier)(value *lval),value *lval)
{
  int lvalue,index;
  cell cidx;

  stgget(&index,&cidx); /* mark position in code generator */
  lvalue=(*hier)(lval);
  if (lval->ident==iCONSTEXPR)
    stgdel(index,cidx); /* load constant later */
  return lvalue;
}

/*  plnge2
 *
 *  Binary plunge to lower level
 *  Called by: plnge(), plnge_rel(), hier14() and hier1()
 */
static void plnge2(void (*oper)(void),void (*arrayoper)(cell),
                   int (*hier)(value *lval),
                   value *lval1,value *lval2)
{
  int index;
  cell cidx;

  stgget(&index,&cidx);             /* mark position in code generator */
  if (lval1->ident==iCONSTEXPR) {   /* constant on left side; it is not yet loaded */
    if (plnge1(hier,lval2))
      rvalue(lval2);                /* load lvalue now */
    else if (lval2->ident==iCONSTEXPR)
      ldconst(lval2->constval*dbltest(oper,lval2,lval1),sPRI);
    ldconst(lval1->constval*dbltest(oper,lval2,lval1),sALT);
                   /* ^ converting constant indices to addresses is restricted
                    *   to "add" and "subtract" operators on array elements */
  } else {                          /* non-constant on left side */
    pushreg(sPRI);
    if (plnge1(hier,lval2))
      rvalue(lval2);
    if (lval2->ident==iCONSTEXPR) { /* constant on right side */
      if (commutative(oper)) {      /* test for commutative operators */
        value lvaltmp = {0};
        stgdel(index,cidx);         /* scratch pushreg() and constant fetch (then
                                     * fetch the constant again */
        ldconst(lval2->constval*dbltest(oper,lval1,lval2),sALT);
        /* now, the primary register has the left operand and the secondary
         * register the right operand; swap the "lval" variables so that lval1
         * is associated with the secondary register and lval2 with the
         * primary register, as is the "normal" case.
         */
        lvaltmp=*lval1;
        *lval1=*lval2;
        *lval2=lvaltmp;
      } else {
        ldconst(lval2->constval*dbltest(oper,lval1,lval2),sPRI);
        popreg(sALT);   /* pop result of left operand into secondary register */
      } /* if */
    } else {            /* non-constants on both sides */
      popreg(sALT);
      if (dbltest(oper,lval1,lval2)>1)
        cell2addr();                    /* double primary register */
      if (dbltest(oper,lval2,lval1)>1)
        cell2addr_alt();                /* double secondary register */
    } /* if */
  } /* if */
  if (oper) {
    cell arraylength=0;
    /* If used in an expression, a function should return a value.
     * If the function has been defined, we can check this. If the
     * function was not defined, we can set this requirement (so that
     * a future function definition can check this bit.
     */
    checkfunction(lval1);
    checkfunction(lval2);
    /* Also, if a function result is used in an expression, assume that it
     * has no side effects. This may not be accurate, but it does allow
     * the compiler to check the effect of the entire expression.
     */
    if (lval1->sym!=NULL && (lval1->sym->ident==iFUNCTN || lval1->sym->ident==iREFFUNC)
        || lval2->sym!=NULL && (lval2->sym->ident==iFUNCTN || lval2->sym->ident==iREFFUNC))
      pc_sideeffect=FALSE;
    if (arrayoper!=NULL
        && (IS_ARRAY(*lval1) || IS_PSEUDO_ARRAY(*lval1))
        && (IS_ARRAY(*lval2) || IS_PSEUDO_ARRAY(*lval2)))
    {
      /* there is an array operator and both sides are an array; test the
       * dimensions first
       */
      arraylength=checkarrays(lval1,lval2);
    } else if (lval1->ident==iARRAY || lval1->ident==iREFARRAY) {
      error(33,check_symbolname(lval1));  /* array must be indexed */
    } else if (lval2->ident==iARRAY || lval2->ident==iREFARRAY) {
      error(33,check_symbolname(lval2));  /* array must be indexed */
    } /* if */
    /* ??? ^^^ should do same kind of error checking with functions */

    /* check whether an "operator" function is defined for the tag names
     * (a constant expression cannot be optimized in that case)
     */
    if (check_userop(oper,lval1->tag,lval2->tag,2,NULL,&lval1->tag)) {
      lval1->ident=iEXPRESSION;
      lval1->constval=0;
    } else if (lval1->ident==iCONSTEXPR && lval2->ident==iCONSTEXPR) {
      /* only constant expression if both constant */
      stgdel(index,cidx);       /* scratch generated code and calculate */
      if (!matchtag(lval1->tag,lval2->tag,FALSE))
        error(213);             /* tagname mismatch */
      lval1->constval=calc(lval1->constval,oper,lval2->constval,&lval1->boolresult);
    } else {
      if (!matchtag(lval1->tag,lval2->tag,FALSE))
        error(213);             /* tagname mismatch */
      if (arraylength>0)
        arrayoper(arraylength*pc_cellsize); /* do the array operation */
      else
        oper();                 /* do the (signed) operation */
      lval1->ident=iEXPRESSION;
    } /* if */
  } /* if */
}

#define IABS(a)       ((a)>=0 ? (a) : (-a))
static cell flooreddiv(cell a,cell b,int return_remainder)
{
  cell q,r;

  if (b==0) {
    error(29);
    return 0;
  } /* if */
  /* first implement truncated division in a portable way */
  q=IABS(a)/IABS(b);
  if ((cell)(a ^ b)<0)
    q=-q;               /* swap sign if either "a" or "b" is negative (but not both) */
  r=a-q*b;              /* calculate the matching remainder */
  /* now "fiddle" with the values to get floored division */
  if (r!=0 && (cell)(r ^ b)<0) {
    q--;
    r+=b;
  } /* if */
  return return_remainder ? r : q;
}

static cell calc(cell left,const void (*oper)(),cell right,char *boolresult)
{
  if (oper==ob_or)
    return (left | right);
  else if (oper==ob_xor)
    return (left ^ right);
  else if (oper==ob_and)
    return (left & right);
  else if (oper==ob_eq)
    return (left == right);
  else if (oper==ob_ne)
    return (left != right);
  else if (oper==os_le)
    return *boolresult &= (char)(left <= right), right;
  else if (oper==os_ge)
    return *boolresult &= (char)(left >= right), right;
  else if (oper==os_lt)
    return *boolresult &= (char)(left < right), right;
  else if (oper==os_gt)
    return *boolresult &= (char)(left > right), right;
  else if (oper==os_sar)
    return (left >> (int)right);
  else if (oper==ou_sar)
    return ((ucell)(left & (~(~(ucell)0 << pc_cellsize*8))) >> (ucell)right);
  else if (oper==ob_sal)
    return ((ucell)left << (int)right);
  else if (oper==ob_add)
    return (left + right);
  else if (oper==ob_sub)
    return (left - right);
  else if (oper==os_mult)
    return (left * right);
  else if (oper==os_div)
    return flooreddiv(left,right,0);
  else if (oper==os_mod)
    return flooreddiv(left,right,1);
  else
    error(29);  /* invalid expression, assumed 0 (this should never occur) */
  return 0;
}

SC_FUNC int expression(cell *val,int *tag,symbol **symptr,int chkfuncresult)
{
  int locheap=decl_heap;
  value lval={0};

  if (hier14(&lval))
    rvalue(&lval);
  /* scrap any arrays left on the heap */
  assert(decl_heap>=locheap);
  modheap((locheap-decl_heap)*pc_cellsize); /* remove heap space, so negative delta */
  decl_heap=locheap;

  if (lval.ident==iCONSTEXPR && val!=NULL)  /* constant expression */
    *val=lval.constval;
  if (tag!=NULL)
    *tag=lval.tag;
  if (symptr!=NULL)
    *symptr=lval.sym;
  if (chkfuncresult)
    checkfunction(&lval);
  return lval.ident;
}

/* returns whether we are currently parsing a preprocessor expression */
static int inside_preproc(void)
{
  /* a preprocessor expression has a special symbol at the end of the string */
  return (strchr((char *)lptr,PREPROC_TERM)!=NULL);
}

SC_FUNC int sc_getstateid(constvalue **automaton,constvalue **state,char *statename)
{
  char name[sNAMEMAX+1],closestmatch[sNAMEMAX+1];
  cell val;
  char *str;
  int fsa,islabel;

  assert(automaton!=NULL);
  assert(state!=NULL);
  if ((islabel=matchtoken(tLABEL))==0 && !needtoken(tSYMBOL))
    return 0;

  tokeninfo(&val,&str);
  assert(strlen(str)<sizeof name);
  strcpy(name,str);
  if (islabel || matchtoken(':')) {
    /* token is an automaton name, add the name and get a new token */
    *automaton=automaton_find(name,closestmatch);
    /* read in the state name before checking the automaton, to keep the parser
     * going (an "unknown automaton" error may occur when the "state" instruction
     * precedes any state definition)
     */
    if (!needtoken(tSYMBOL))
      return 0;
    tokeninfo(&val,&str);        /* do not copy the name yet, must check automaton first */
    if (*automaton==NULL) {
      if (*closestmatch!='\0')
        error(makelong(86,1),name,closestmatch);
      else
        error(86,name);          /* unknown automaton */
      return 0;
    } /* if */
    assert((*automaton)->index>0);
    assert(strlen(str)<sizeof name);
    strcpy(name,str);
  } else {
    *automaton=automaton_find("",NULL);
    assert(*automaton!=NULL);
    assert((*automaton)->index==0);
  } /* if */
  assert(*automaton!=NULL);
  fsa=(*automaton)->index;

  if (statename!=NULL)
    strcpy(statename,name);

  *state=state_find(name,fsa,closestmatch);
  if (*state==NULL) {
    char *fsaname=(*automaton)->name;
    if (*fsaname=='\0')
      fsaname="<main>";
    if (*closestmatch!='\0')
      error(makelong(87,1),name,fsaname,closestmatch);
    else
      error(87,name,fsaname); /* unknown state for automaton */
    return 0;
  } /* if */

  return 1;
}

SC_FUNC cell array_totalsize(symbol *sym)
{
  cell length;

  assert(sym!=NULL);
  assert(sym->ident==iARRAY || sym->ident==iREFARRAY);
  length=sym->dim.array.length;
  if (sym->dim.array.level > 0) {
    cell sublength=array_totalsize(finddepend(sym));
    if (sublength>0)
      length=length+length*sublength;
    else
      length=0;
  } /* if */
  return length;
}

static cell array_levelsize(symbol *sym,int level)
{
  assert(sym!=NULL);
  assert(sym->ident==iARRAY || sym->ident==iREFARRAY);
  assert(level <= sym->dim.array.level);
  while (level-- > 0) {
    sym=finddepend(sym);
    assert(sym!=NULL);
  } /* if */
  return sym->dim.array.length;
}

/*  hier14
 *
 *  Lowest hierarchy level (except for the , operator).
 *
 *  Global references: sc_intest        (reffered to only)
 *                     sc_allowproccall (modified)
 */
static int hier14(value *lval1)
{
  int lvalue;
  value lval2={0},lval3={0};
  void (*oper)(void);
  int tok,i;
  cell val;
  char *st;
  int bwcount,leftarray;
  cell arrayidx1[sDIMEN_MAX],arrayidx2[sDIMEN_MAX];  /* last used array indices */
  cell *org_arrayidx;

  bwcount=bitwise_opercount;
  bitwise_opercount=0;
  /* initialize the index arrays with unlikely constant indices; note that
   * these indices will only be changed when the array is indexed with a
   * constant, and that negative array indices are invalid (so actually, any
   * negative value would do).
   */
  for (i=0; i<sDIMEN_MAX; i++)
    arrayidx1[i]=arrayidx2[i]=(cell)((ucell)~0UL << (pc_cellsize*8-1));
  org_arrayidx=lval1->arrayidx; /* save current pointer, to reset later */
  if (lval1->arrayidx==NULL)
    lval1->arrayidx=arrayidx1;
  lvalue=plnge1(hier13,lval1);
  if (lval1->ident!=iARRAYCELL && lval1->ident!=iARRAYCHAR)
    lval1->arrayidx=NULL;
  if (lval1->ident==iCONSTEXPR) /* load constant here */
    ldconst(lval1->constval,sPRI);
  tok=lex(&val,&st);
  switch (tok) {
    case taOR:
      oper=ob_or;
      break;
    case taXOR:
      oper=ob_xor;
      break;
    case taAND:
      oper=ob_and;
      break;
    case taADD:
      oper=ob_add;
      break;
    case taSUB:
      oper=ob_sub;
      break;
    case taMULT:
      oper=os_mult;
      break;
    case taDIV:
      oper=os_div;
      break;
    case taMOD:
      oper=os_mod;
      break;
    case taSHRU:
      oper=ou_sar;
      break;
    case taSHR:
      oper=os_sar;
      break;
    case taSHL:
      oper=ob_sal;
      break;
    case '=':           /* simple assignment */
      oper=NULL;
      if (sc_intest)
        error(211);     /* possibly unintended assignment */
      break;
    default:
      lexpush();
      bitwise_opercount=bwcount;
      lval1->arrayidx=org_arrayidx; /* restore array index pointer */
      return lvalue;
  } /* switch */

  /* if we get here, it was an assignment; first check a few special cases
   * and then the general */
  if (lval1->ident==iARRAYCHAR) {
    /* special case, assignment to packed character in a cell is permitted */
    lvalue=TRUE;
  } else if (lval1->ident==iARRAY || lval1->ident==iREFARRAY) {
    /* array assignment is permitted too (with restrictions) */
    if (oper)
      return error(23); /* array assignment must be simple assigment */
    if (lval1->sym==NULL)
      return error(22); /* must be an lvalue */
    lvalue=TRUE;
  } /* if */

  /* operand on left side of assignment must be lvalue */
  sc_allowproccall=FALSE;       /* may no longer use "procedure call" syntax */
  if (!lvalue) {
    hier14(&lval2);             /* gobble up right-hand of the operator */
    return error(22);           /* must be lvalue */
  } /* if */
  /* may not change "constant" parameters */
  assert(lval1->sym!=NULL);
  if ((lval1->sym->usage & uCONST)!=0) {
    hier14(&lval2);             /* gobble up right-hand of the operator */
    return error(22);           /* assignment to const argument */
  } /* if */

  lval3=*lval1;         /* save symbol to enable storage of expresion result */
  lval1->arrayidx=org_arrayidx; /* restore array index pointer */
  if (lval1->ident==iARRAYCELL || lval1->ident==iARRAYCHAR
      || lval1->ident==iARRAY || lval1->ident==iREFARRAY)
  {
    /* if indirect fetch: save PRI (cell address) */
    if (oper) {
      pushreg(sPRI);
      rvalue(lval1);
    } /* if */
    lval2.arrayidx=arrayidx2;
    plnge2(oper,NULL,hier14,lval1,&lval2);
    if (lval2.ident!=iARRAYCELL && lval2.ident!=iARRAYCHAR)
      lval2.arrayidx=NULL;
    if (oper)
      popreg(sALT);
    if (!oper && lval3.arrayidx!=NULL && lval2.arrayidx!=NULL
        && lval3.ident==lval2.ident && lval3.sym==lval2.sym)
    {
      int same=TRUE;
      assert(lval2.arrayidx==arrayidx2);
      for (i=0; i<sDIMEN_MAX; i++)
        same=same && (lval3.arrayidx[i]==lval2.arrayidx[i]);
        if (same)
          error(226,lval3.sym->name);   /* self-assignment */
    } /* if */
  } else {
    if (oper){
      rvalue(lval1);
      plnge2(oper,NULL,hier14,lval1,&lval2);
    } else {
      /* if direct fetch and simple assignment: no "push"
       * and "pop" needed -> call hier14() directly, */
      if (hier14(&lval2))
        rvalue(&lval2);         /* instead of plnge2() */
      else if (lval2.ident==iVARIABLE)
        lval2.ident=iEXPRESSION;/* mark as "rvalue" if it is not an "lvalue" */
      checkfunction(&lval2);
      /* check whether lval2 and lval3 (old lval1) refer to the same variable */
      if (lval2.ident==iVARIABLE && lval3.ident==lval2.ident && lval3.sym==lval2.sym) {
        assert(lval3.sym!=NULL);
        error(226,lval3.sym->name);     /* self-assignment */
      } /* if */
    } /* if */
  } /* if */
  /* Array elements are sometimes considered as sub-arrays --when named
   * indices are in effect and the named "field size" is greater than 1.
   * If the expression on the right side of the assignment is a cell,
   * or if an operation is in effect, this does not apply.
   */
  leftarray= IS_ARRAY(lval3)
             || (IS_PSEUDO_ARRAY(lval3) && !oper && (lval2.ident==iARRAY || lval2.ident==iREFARRAY));
  if (leftarray) {
    /* Left operand is an array, right operand should be an array variable
     * of the same size and the same dimension, an array literal (of the
     * same size) or a literal string. For single-dimensional arrays without
     * tag for the index, it is permitted to assign a smaller array into a
     * larger one (without warning). This is to make it easier to work with
     * strings. Function checkarrays() verifies all this.
     */
    val=checkarrays(&lval3,&lval2);
  } else {
    /* left operand is not an array, right operand should then not be either */
    if (lval2.ident==iARRAY || lval2.ident==iREFARRAY)
      error(6);         /* must be assigned to an array */
  } /* if */
  if (leftarray) {
    /* If the arrays are single-dimensional, we can simply copy the data; if
     * both arrays are at their roots (like "a = b", where "a" and "b" are
     * arrays), a copy of the full array (including the indirection vectors)
     * can also be done. However, when doing something like "a[2] = b" where
     * "b" is two-dimensional, we need to be caureful about the indirection
     * vectors in "a": we cannot copy the indirection vector of "b", because
     * the corresponding one of "a" is at a different offset.
     */
    assert(lval3.sym!=NULL);
    if (lval3.sym->dim.array.level==0
        || (lval2.sym!=NULL && lval3.sym->parent==NULL && lval2.sym->parent==NULL))
    {
      memcopy(val*pc_cellsize);
    } else {
      symbol *subsym=finddepend(lval3.sym);
      assert(subsym!=NULL);
      copyarray2d((int)lval3.sym->dim.array.length,(int)subsym->dim.array.length);
      #if sDIMEN_MAX > 3
        #error Copying partial arrays with more than 2 dimensions is not yet implemented
      #endif
    } /* if */
  } else {
    check_userop(NULL,lval2.tag,lval3.tag,2,&lval3,&lval2.tag);
    store(&lval3);      /* now, store the expression result */
  } /* if */
  if (!oper && !matchtag(lval3.tag,lval2.tag,TRUE))
    error(213);         /* tagname mismatch (if "oper", warning already given in plunge2()) */
  if (lval3.sym)
    markusage(lval3.sym,uWRITTEN);
  pc_sideeffect=TRUE;
  bitwise_opercount=bwcount;
  lval1->ident=iEXPRESSION;
  return FALSE;         /* expression result is never an lvalue */
}

static int hier13(value *lval)
{
  int lvalue=plnge1(hier12,lval);
  if (matchtoken('?')) {
    int locheap=decl_heap;      /* save current heap delta */
    long heap1,heap2;           /* max. heap delta either branch */
    valuepair *heaplist_node;
    int flab1=getlabel();
    int flab2=getlabel();
    value lval2={0};
    int array1,array2;
    short save_allowtags;

    if (lvalue) {
      rvalue(lval);
    } else if (lval->ident==iCONSTEXPR) {
      ldconst(lval->constval,sPRI);
      error(lval->constval ? 206 : 205);        /* redundant test */
    } else {
      checkfunction(lval);      /* if the test is a function, that function
                                 * should return a value */
    } /* if */
    heap1=heap2=0;				/* just to avoid a compiler warning */
    if (sc_status==statBROWSE) {
      /* We should push a new node right now otherwise we will pop it in the
       * wrong order on the write stage.
       */
      heaplist_node=push_heaplist(0,0); /* save the pointer to write the actual data later */
    } else if (sc_status==statWRITE || sc_status==statSKIP) {
      #if !defined NDEBUG
        int result=
      #endif
      popfront_heaplist(&heap1,&heap2);
      assert(result);           /* pop off equally many items than were pushed */
    } /* if */
    jmp_eq0(flab1);             /* go to second expression if primary register==0 */
    save_allowtags=sc_allowtags;
    sc_allowtags=FALSE;         /* do not allow tagnames here (colon is a special token) */
    if (sc_status==statWRITE) {
      modheap(heap1*pc_cellsize);
      decl_heap+=heap1;         /* equilibrate the heap (see comment below) */
    } /* if */
    if (hier13(lval))
      rvalue(lval);
    if (lval->ident==iCONSTEXPR)        /* load constant here */
      ldconst(lval->constval,sPRI);
    sc_allowtags=save_allowtags;/* restore */
    heap1=decl_heap-locheap;    /* save heap space used in "true" branch */
    assert(heap1>=0);
    decl_heap=locheap;          /* restore heap delta */
    jumplabel(flab2);
    setlabel(flab1);
    needtoken(':');
    if (sc_status==statWRITE) {
      modheap(heap2*pc_cellsize);
      decl_heap+=heap2;         /* equilibrate the heap (see comment below) */
    } /* if */
    if (hier13(&lval2))
      rvalue(&lval2);
    if (lval2.ident==iCONSTEXPR)        /* load constant here */
      ldconst(lval2.constval,sPRI);
    heap2=decl_heap-locheap;    /* save heap space used in "false" branch */
    assert(heap2>=0);
    array1= (lval->ident==iARRAY || lval->ident==iREFARRAY);
    array2= (lval2.ident==iARRAY || lval2.ident==iREFARRAY);
    if (array1 && !array2) {
      error(33,check_symbolname(lval)); /* array must be indexed */
    } else if (!array1 && array2) {
      error(33,check_symbolname(&lval2)); /* array must be indexed */
    } else if (array1 && array2) {
      long length1,length2,level1,level2;
      if (lval->sym!=NULL) {
        length1=(lval->constval==0) ? (long)lval->sym->dim.array.length : (long)lval->constval;
        level1=lval->sym->dim.array.level;
      } else {
        length1=(long)lval->constval;
        level1=0;
      } /* if */
      if (lval2.sym!=NULL) {
        length2=(lval2.constval==0) ? (long)lval2.sym->dim.array.length : (long)lval2.constval;
        level2=lval2.sym->dim.array.level;
      } else {
        length2=(long)lval2.constval;
        level2=0;
      } /* if */
      if (level1!=level2)
        error(48);                      /* array dimensions do not match */
      else if (lval->sym!=NULL && lval->sym->dim.array.names!=NULL
               && (lval2.sym==NULL || lval2.sym->dim.array.names==NULL || !compare_consttable(lval->sym->dim.array.names,lval2.sym->dim.array.names)))
        error(47);                      /* array definitions do not match */
      if (level1==0 && level2==0) {
        /* for arrays with only one dimension, keep the maximum length */
        if (IABS(length1)<IABS(length2))
          lval->constval=length2;
      } else if (level1>0 && level2>0) {
        /* for arrays with multiple dimensions, both arrays must match exactly */
        assert(lval->sym!=NULL);
        assert(lval2.sym!=NULL);
        checkarrays(lval,&lval2);
      } /* if */
    } /* if */
    if (!matchtag(lval->tag,lval2.tag,FALSE))
      error(213);               /* tagname mismatch ('true' and 'false' expressions) */
    setlabel(flab2);
    if (sc_status==statBROWSE) {
      /* Calculate the max. heap space used by either branch and save values of
       * max - heap1 and max - heap2. On the second pass, we use these values
       * to equilibrate the heap space used by either branch. This is needed
       * because we don't know (at compile time) which branch will be taken,
       * but the heap cannot be restored inside each branch because the result
       * on the heap may needed by the remaining expression.
       */
      int max=(heap1>heap2) ? heap1 : heap2;
      heaplist_node->first=max-heap1;
      heaplist_node->second=max-heap2;
      decl_heap=locheap+max; /* otherwise it will contain locheap+heap2 and the
                              * max. heap usage will be wrong for the upper
                              * expression */
    } /* if */
    assert(sc_status!=statWRITE || heap1==heap2);
    if (lval->ident==iARRAY)
      lval->ident=iREFARRAY;    /* iARRAY becomes iREFARRAY */
    else if (lval->ident!=iREFARRAY)
      lval->ident=iEXPRESSION;  /* iREFARRAY stays iREFARRAY, rest becomes iEXPRESSION */
    pc_sideeffect=FALSE;        /* assume any of the sub-expressions had no side effect,
                                 * so that a warning is given if the result of the
                                 * expression is unused */
    return FALSE;               /* conditional expression is no lvalue */
  } else {
    return lvalue;
  } /* if */
}

/* the order of the operators in these lists is important and must be
 * the same as the order of the operators in the array "op1" (with the
 * exception of list11 and list12, because these "early-exit" operators
 * are a special case anyway)
 */
static const int list3[]  = {'*','/','%',0};
static const int list4[]  = {'+','-',0};
static const int list5[]  = {tSHL,tSHR,tSHRU,0};
static const int list6[]  = {'&',0};
static const int list7[]  = {'^',0};
static const int list8[]  = {'|',0};
static const int list9[]  = {tlLE,tlGE,'<','>',0};
static const int list10[] = {tlEQ,tlNE,0};
static const int list11[] = {tlAND,0};
static const int list12[] = {tlOR,0};

static int hier12(value *lval)
{
  return skim(list12,jmp_ne0,1,0,hier11,lval);
}

static int hier11(value *lval)
{
  return skim(list11,jmp_eq0,0,1,hier10,lval);
}

static int hier10(value *lval)
{ /* ==, != */
  return plnge(list10,15,hier9,lval,"bool",TRUE);
}                  /* ^ this variable is the starting index in the op1[]
                    *   array of the operators of this hierarchy level */

static int hier9(value *lval)
{ /* <=, >=, <, > */
  return plnge_rel(list9,11,hier8,lval);
}

static int hier8(value *lval)
{ /* | */
  return plnge(list8,10,hier7,lval,NULL,FALSE);
}

static int hier7(value *lval)
{ /* ^ */
  return plnge(list7,9,hier6,lval,NULL,FALSE);
}

static int hier6(value *lval)
{ /* & */
  return plnge(list6,8,hier5,lval,NULL,FALSE);
}

static int hier5(value *lval)
{ /* <<, >>, >>> */
  return plnge(list5,5,hier4,lval,NULL,FALSE);
}

static int hier4(value *lval)
{ /* +, - */
  return plnge(list4,3,hier3,lval,NULL,FALSE);
}

static int hier3(value *lval)
{ /* *, /, % */
  return plnge(list3,0,hier2,lval,NULL,FALSE);
}

static int hier2(value *lval)
{
  int lvalue,tok;
  int tag,paranthese;
  cell val;
  char *st;
  symbol *sym;
  short save_allowtags;

  tok=lex(&val,&st);
  switch (tok) {
  case tINC:                    /* ++lval */
    if (matchtoken(tNEW)) {
      lexpush();                /* to avoid subsequent "shadowing" warnings */
      return error(22);         /* must be lvalue */
    } /* if */
    if (!hier2(lval))
      return error(22);         /* must be lvalue */
    assert(lval->sym!=NULL);
    if ((lval->sym->usage & uCONST)!=0)
      return error(22);         /* assignment to const argument */
    if (!check_userop(user_inc,lval->tag,0,1,lval,&lval->tag))
      inc(lval);                /* increase variable first */
    rvalue(lval);               /* and read the result into PRI */
    pc_sideeffect=TRUE;
    return FALSE;               /* result is no longer lvalue */
  case tDEC:                    /* --lval */
    if (!hier2(lval))
      return error(22);         /* must be lvalue */
    assert(lval->sym!=NULL);
    if ((lval->sym->usage & uCONST)!=0)
      return error(22);         /* assignment to const argument */
    if (!check_userop(user_dec,lval->tag,0,1,lval,&lval->tag))
      dec(lval);                /* decrease variable first */
    rvalue(lval);               /* and read the result into PRI */
    pc_sideeffect=TRUE;
    return FALSE;               /* result is no longer lvalue */
  case '~':                     /* ~ (one's complement) */
    if (hier2(lval))
      rvalue(lval);
    else if (lval->ident==iARRAY || lval->ident==iREFARRAY)
      error(33,check_symbolname(lval)); /* array was not indexed in an expression */
    invert();                   /* bitwise NOT */
    lval->constval=~lval->constval;
    if (lval->ident!=iCONSTEXPR)
      lval->ident=iEXPRESSION;
    return FALSE;
  case '!':                     /* ! (logical negate) */
    if (hier2(lval))
      rvalue(lval);
    else if (lval->ident==iARRAY || lval->ident==iREFARRAY)
      error(33,check_symbolname(lval)); /* array was not indexed in an expression */
    if (check_userop(lneg,lval->tag,0,1,NULL,&lval->tag)) {
      lval->ident=iEXPRESSION;
      lval->constval=0;
    } else {
      lneg();                   /* 0 -> 1,  !0 -> 0 */
      lval->constval=!lval->constval;
      lval->tag=pc_addtag("bool");
      if (lval->ident!=iCONSTEXPR)
        lval->ident=iEXPRESSION;
    } /* if */
    return FALSE;
  case '-':                     /* unary - (two's complement) */
    if (hier2(lval))
      rvalue(lval);
    else if (lval->ident==iARRAY || lval->ident==iREFARRAY)
      error(33,check_symbolname(lval)); /* array was not indexed in an expression */
    /* make a special check for a constant expression with the tag of a
     * rational number, so that we can simple swap the sign of that constant.
     */
    if (lval->ident==iCONSTEXPR && lval->tag==sc_rationaltag && sc_rationaltag!=0) {
      if (rational_digits==0) {
        assert(pc_cellsize==4 || pc_cellsize==8);
        if (pc_cellsize==4) {
          float *f = (float *)&lval->constval;
          *f= - *f;     /* this modifies lval->constval */
        } else {
          double *f = (double *)&lval->constval;
          *f= - *f;     /* this modifies lval->constval */
        } /* if */
      } else {
        /* the negation of a fixed point number is just an integer negation */
        lval->constval=-lval->constval;
      } /* if */
    } else if (check_userop(neg,lval->tag,0,1,NULL,&lval->tag)) {
      lval->ident=iEXPRESSION;
      lval->constval=0;
    } else {
      neg();                    /* arithmic negation */
      lval->constval=-lval->constval;
      if (lval->ident!=iCONSTEXPR)
        lval->ident=iEXPRESSION;
    } /* if */
    return FALSE;
  case tLABEL:                  /* tagname override */
    tag=pc_addtag(st);
    lvalue=hier2(lval);
    lval->tag=tag;
    return lvalue;
  case tDEFINED:
    paranthese=0;
    while (matchtoken('('))
      paranthese++;
    tok=lex(&val,&st);
    if (tok!=tSYMBOL)
      return error_suggest(20,st,iVARIABLE);  /* illegal symbol name */
    sym=findloc(st);
    if (sym==NULL)
      sym=findglb(st,sSTATEVAR);
    if (sym!=NULL
        && (((sym->usage & uDEFINE)==0 && sym->ident!=iFUNCTN)
            || ((sym->ident==iFUNCTN || sym->ident==iREFFUNC) && (sym->usage & uPROTOTYPED)==0)))
      sym=NULL;                 /* symbol is in the table, but not as "defined" or "prototyped" */
    val= (sym!=NULL);
    if (!val && find_subst(st,(int)strlen(st))!=NULL)
      val=1;
    if (!val)
      insert_undefsymbol(st,pc_curline); /* if the symbol is not defined, add it to the "undefined symbols" list */
    clear_value(lval);
    lval->ident=iCONSTEXPR;
    lval->constval= val;
    lval->tag=pc_addtag("bool");
    ldconst(lval->constval,sPRI);
    while (paranthese--)
      needtoken(')');
    return FALSE;
  case tSIZEOF:
    paranthese=0;
    while (matchtoken('('))
      paranthese++;
    tok=lex(&val,&st);
    if (tok!=tSYMBOL)
      return error_suggest(20,st,iVARIABLE);  /* illegal symbol name */
    sym=findloc(st);
    if (sym==NULL)
      sym=findglb(st,sSTATEVAR);
    if (sym==NULL)
      return error_suggest(17,st,iVARIABLE);  /* undefined symbol */
    if (sym->ident==iCONSTEXPR)
      error(39);                /* constant symbol has no size */
    else if (sym->ident==iFUNCTN || sym->ident==iREFFUNC)
      error(72);                /* "function" symbol has no size */
    else if ((sym->usage & uDEFINE)==0)
      return error_suggest(17,st,iVARIABLE); /* undefined symbol (symbol is in the table, but it is "used" only) */
    clear_value(lval);
    lval->ident=iCONSTEXPR;
    lval->constval=1;           /* preset */
    if (sym->ident==iARRAY || sym->ident==iREFARRAY) {
      int level,symlabel;
      char idxname[sNAMEMAX+1],*ptr;
      constvalue *namelist=NULL;
      symbol *subsym=sym;
      for (level=0; (symlabel=matchtoken(tSYMLABEL))!=0 || matchtoken('['); level++) {
        namelist=NULL;
        if (subsym!=NULL && (symlabel || matchtoken(tSYMLABEL))) {
          namelist=subsym->dim.array.names;
          if (namelist!=NULL) {
            tokeninfo(&val,&ptr);
            assert(strlen(ptr)<sizearray(idxname));
            strcpy(idxname,ptr);
          } else {
            error(94,sym->name);
          } /* if */
        } /* if */
        if (!symlabel)
          needtoken(']');
        if (subsym!=NULL)
          subsym=finddepend(subsym);
      } /* for */
      if (level>sym->dim.array.level+1) {
        error(28,sym->name);  /* invalid subscript */
      } else if (level==sym->dim.array.level+1 && namelist!=NULL) {
        constvalue *item=find_constval(namelist,idxname,-1);
        if (item==NULL)
          error_suggest_list(80,idxname,namelist);
        else if (item->next!=NULL)
          lval->constval=item->next->value - item->value;
      } else {
        lval->constval=array_levelsize(sym,level);
      } /* if */
      if (lval->constval==0 && !inside_preproc())
        error(224,st);          /* indeterminate array size in "sizeof" expression */
    } /* if */
    ldconst(lval->constval,sPRI);
    while (paranthese--)
      needtoken(')');
    return FALSE;
  case tTAGOF:
    save_allowtags=sc_allowtags;
    paranthese=0;
    while (matchtoken('(')) {
      paranthese++;
      sc_allowtags=TRUE;        /* allow tagnames to be used in parenthesized expressions */
    }
    tok=lex(&val,&st);
    if (tok!=tSYMBOL && tok!=tLABEL)
      return error_suggest(20,st,iVARIABLE);  /* illegal symbol name */
    if (tok==tLABEL) {
      constvalue *tagsym=find_constval(&tagname_tab,st,-1);
      tag=(int)((tagsym!=NULL) ? tagsym->value : 0);
      sym=NULL;
    } else {
      sym=findloc(st);
      if (sym==NULL)
        sym=findglb(st,sSTATEVAR);
      if (sym==NULL)
        return error_suggest(17,st,iVARIABLE); /* undefined symbol */
      if ((sym->usage & uDEFINE)==0)
        return error_suggest(17,st,iVARIABLE); /* undefined symbol (symbol is in the table, but it is "used" only) */
      tag=sym->tag;
    } /* if */
    if (sym!=NULL && (sym->ident==iARRAY || sym->ident==iREFARRAY)) {
      int level,symlabel;
      char idxname[sNAMEMAX+1],*ptr;
      constvalue *namelist=NULL;
      symbol *subsym=sym;
      for (level=0; (symlabel=matchtoken(tSYMLABEL))!=0 || matchtoken('['); level++) {
        namelist=NULL;
        if (subsym!=NULL && (symlabel || matchtoken(tSYMLABEL))) {
          namelist=subsym->dim.array.names;
          if (namelist!=NULL) {
            tokeninfo(&val,&ptr);
            assert(strlen(ptr)<sizearray(idxname));
            strcpy(idxname,ptr);
          } else {
            error(94,sym->name);
          } /* if */
        } /* if */
        if (!symlabel)
          needtoken(']');
        if (subsym!=NULL)
          subsym=finddepend(subsym);
      } /* for */
      if (level>sym->dim.array.level+1) {
        error(28,sym->name);  /* invalid subscript */
      } else if (level==sym->dim.array.level+1 && namelist!=NULL) {
        constvalue *item=find_constval(namelist,idxname,-1);
        if (item==NULL)
          error_suggest_list(80,idxname,namelist);
        else if (item->index!=0)
          tag=item->index;
      } /* if */
    } /* if */
    if (tag!=0)
      exporttag(tag);
    clear_value(lval);
    lval->ident=iCONSTEXPR;
    lval->constval= (tag==0) ? tag : tag | PUBLICTAG;
    ldconst(lval->constval,sPRI);
    while (paranthese--)
      needtoken(')');
    sc_allowtags=save_allowtags;
    return FALSE;
  case tSTATE: {
    constvalue *automaton;
    constvalue *state;
    if (sc_getstateid(&automaton,&state,NULL)) {
      assert(automaton!=NULL);
      assert(automaton->index==0 && automaton->name[0]=='\0' || automaton->index>0);
      loadreg(automaton->value,sALT);
      assert(state!=NULL);
      ldconst(state->value,sPRI);
      ob_eq();
      clear_value(lval);
      lval->ident=iEXPRESSION;
      lval->tag=pc_addtag("bool");
    } /* if */
    return FALSE;
  } /* case */
  default:
    lexpush();
    lvalue=hier1(lval);
    /* check for postfix operators */
    if (matchtoken(';')) {
      /* Found a ';', do not look further for postfix operators */
      lexpush();                /* push ';' back after successful match */
      return lvalue;
    } else if (matchtoken(tTERM)) {
      /* Found a newline that ends a statement (this is the case when
       * semicolons are optional). Note that an explicit semicolon was
       * handled above. This case is similar, except that the token must
       * not be pushed back.
       */
      return lvalue;
    } else {
      int saveresult;
      tok=lex(&val,&st);
      switch (tok) {
      case tINC:                /* lval++ */
        if (!lvalue)
          return error(22);     /* must be lvalue */
        assert(lval->sym!=NULL);
        if ((lval->sym->usage & uCONST)!=0)
          return error(22);     /* assignment to const argument */
        /* on incrementing array cells, the address in PRI must be saved for
         * incremening the value, whereas the current value must be in PRI
         * on exit.
         */
        saveresult= (lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR);
        if (saveresult)
          pushreg(sPRI);        /* save address in PRI */
        rvalue(lval);           /* read current value into PRI */
        if (saveresult)
          swap1();              /* save PRI on the stack, restore address in PRI */
        if (!check_userop(user_inc,lval->tag,0,1,lval,&lval->tag))
          inc(lval);            /* increase variable afterwards */
        if (saveresult)
          popreg(sPRI);         /* restore PRI (result of rvalue()) */
        pc_sideeffect=TRUE;
        return FALSE;           /* result is no longer lvalue */
      case tDEC:                /* lval-- */
        if (!lvalue)
          return error(22);     /* must be lvalue */
        assert(lval->sym!=NULL);
        if ((lval->sym->usage & uCONST)!=0)
          return error(22);     /* assignment to const argument */
        saveresult= (lval->ident==iARRAYCELL || lval->ident==iARRAYCHAR);
        if (saveresult)
          pushreg(sPRI);        /* save address in PRI */
        rvalue(lval);           /* read current value into PRI */
        if (saveresult)
          swap1();              /* save PRI on the stack, restore address in PRI */
        if (!check_userop(user_dec,lval->tag,0,1,lval,&lval->tag))
          dec(lval);            /* decrease variable afterwards */
        if (saveresult)
          popreg(sPRI);         /* restore PRI (result of rvalue()) */
        pc_sideeffect=TRUE;
        return FALSE;
      default:
        lexpush();
        return lvalue;
      } /* switch */
    } /* if */
  } /* switch */
}

/*  hier1
 *
 *  The highest hierarchy level: it looks for pointer and array indices
 *  and function calls.
 *  Generates code to fetch a pointer value if it is indexed and code to
 *  add to the pointer value or the array address (the address is already
 *  read at primary()). It also generates code to fetch a function address
 *  if that hasn't already been done at primary() (check lval[4]) and calls
 *  callfunction() to call the function.
 */
static int hier1(value *lval1)
{
  int lvalue,index,tok,symtok;
  cell val,cidx;
  value lval2={0};
  char *symlabel;
  int close,optbrackets;
  symbol *sym,*cursym;
  symbol dummysymbol; /* for plunging into pseudo-arrays */

  lvalue=primary(lval1,&symtok);
  cursym=lval1->sym;
restart:
  sym=cursym;
  if (matchtoken('[') || matchtoken('{') || matchtoken('(') || matchtoken(tSYMLABEL)) {
    tok=tokeninfo(&val,&symlabel);      /* get token read by matchtoken() */
    if (sym==NULL && symtok!=tSYMBOL || lval1->ident==iEXPRESSION) {
      /* we do not have a valid symbol and we appear not to have read a valid
       * symbol name (so it is unlikely that we would have read a name of an
       * undefined symbol)
       */
      if (lval1->ident!=iEXPRESSION || tok!='{') /* no warning for '{', assume the expression had ended */
        error(29);              /* expression error, assumed 0 */
      lexpush();                /* analyse '(', '{' or '[' again later */
      return FALSE;
    } /* if */
    optbrackets=(sym!=NULL && (sym->ident==iARRAY || sym->ident==iREFARRAY));
    if (tok=='[' || tok=='{' || (tok==tSYMLABEL && optbrackets)) { /* subscript */
      constvalue *cval=NULL;
      switch (tok) {
      case '[':
        close=']';
        break;
      case '{':
        close='}';
        break;
      case tSYMLABEL:
        close=tSYMLABEL;
        break;
      default:
        assert(0);
        close=0;
      } /* switch */
      if (sym==NULL) {
        /* sym==NULL if lval is a constant or a literal, or an unknown variable */
        if (strlen(lastsymbol)>0)
          error_suggest(28,lastsymbol,iARRAY);  /* cannot subscript */
        else
          error(28,"<unknown>");
        if (close!=tSYMLABEL)
          skiptotoken(close);
        return FALSE;
      } else if (sym->ident!=iARRAY && sym->ident!=iREFARRAY){
        error_suggest(28,sym->name,iARRAY);   /* cannot subscript */
        if (close!=tSYMLABEL)
          skiptotoken(close);
        return FALSE;
      } else if (sym->dim.array.level>0 && close=='}') {
        error(51);              /* invalid subscript, must use [ ] */
      } /* if */
      stgget(&index,&cidx);     /* mark position in code generator */
      pushreg(sPRI);            /* save base address of the array */
      if (close==tSYMLABEL || matchtoken(tSYMLABEL)) {
        if (close!=tSYMLABEL)
          tokeninfo(&val,&symlabel);
        if (sym->dim.array.names!=NULL)
          cval=find_constval(sym->dim.array.names,symlabel,-1);
        if (cval==NULL) {
          char msg[2*sNAMEMAX+2];
          sprintf(msg,"%s.%s",sym->name,symlabel);
          error(94,msg);        /* invalid subscript (named index isn't found, or no named indices defined) */
        } else if (close=='}') {
          error(51);            /* invalid subscript ([] is required for named indices) */
        } else {
          /* code to fetch the constant is not generated here, as we will drop
           * in the general case for "constant indices" later
           */
          lval2.ident=iCONSTEXPR;
          lval2.constval=cval->value;
          lval2.tag=cval->index;
          lval2.sym=NULL;
          lval2.ispacked=((cval->usage & uPACKED)!=0);
        } /* if */
      } else {
        if (sym->dim.array.names!=NULL && !IS_PSEUDO_ARRAY(*lval1))
          error(94,sym->name);  /* invalid subscript (named indices should be used) */
        if ((sym->usage & uPACKED)==0 && close=='}' || (sym->usage & uPACKED)!=0 && (close==']' || close==tSYMLABEL))
          error(229);
        if (hier14(&lval2))     /* create expression for the array index */
          rvalue(&lval2);
        if (lval2.ident==iARRAY || lval2.ident==iREFARRAY)
          error(33,lval2.sym->name);    /* array must be indexed */
      } /* if */
      if (close!=tSYMLABEL)
        needtoken(close);
      if (lval2.ident==iCONSTEXPR) {    /* constant expression */
        stgdel(index,cidx);             /* scratch generated code */
        if (lval1->arrayidx!=NULL) {    /* keep constant index, for checking */
          assert(sym->dim.array.level>=0 && sym->dim.array.level<sDIMEN_MAX);
          lval1->arrayidx[sym->dim.array.level]=lval2.constval;
        } /* if */
        if (close!='}') {
          /* normal array index (or optional [ ]) */
          if (lval2.constval<0 || sym->dim.array.length!=0 && sym->dim.array.length<=lval2.constval)
            error(32,sym->name);        /* array index out of bounds */
          if (lval2.constval!=0) {
            /* don't add offsets for zero subscripts */
            if (pc_cellsize==2) {
              ldconst(lval2.constval<<1,sALT);
            } else if (pc_cellsize==4) {
              ldconst(lval2.constval<<2,sALT);
            } else {
              assert(pc_cellsize==8);
              ldconst(lval2.constval<<3,sALT);
            } /* if */
            ob_add();
          } /* if */
        } else {
          /* character index */
          if (lval2.constval<0 || sym->dim.array.length!=0
              && sym->dim.array.length*((8*pc_cellsize)/sCHARBITS)<=lval2.constval)
            error(32,sym->name);        /* array index out of bounds */
          if (lval2.constval!=0) {
            /* don't add offsets for zero subscripts */
            #if sCHARBITS==16
              ldconst(lval2.constval<<1,sALT);/* 16-bit character */
            #else
              ldconst(lval2.constval,sALT);   /* 8-bit character */
            #endif
            ob_add();
          } /* if */
          charalign();                  /* align character index into array */
        } /* if */
      } else {
        /* array index is not constant (so brackets are never optional) */
        lval1->arrayidx=NULL;           /* reset, so won't be checked */
        if (close==']') {
          if (sym->dim.array.length!=0)
            ffbounds(sym->dim.array.length-1);  /* run time check for array bounds */
          cell2addr();  /* normal array index */
        } else {
          if (sym->dim.array.length!=0)
            ffbounds(sym->dim.array.length*((8*pc_cellsize)/sCHARBITS)-1);
          char2addr();  /* character array index */
        } /* if */
        popreg(sALT);
        ob_add();       /* base address was popped into secondary register */
        if (close!=']')
          charalign();  /* align character index into array */
      } /* if */
      /* the indexed item may be another array (multi-dimensional arrays) */
      assert(cursym==sym && sym!=NULL); /* should still be set */
      if (sym->dim.array.level>0) {
        assert(close==']');     /* checked earlier */
        assert(cursym==lval1->sym);
        /* read the offset to the subarray and add it to the current address */
        lval1->ident=iARRAYCELL;
        pushreg(sPRI);          /* the optimizer makes this to a MOVE.alt */
        rvalue(lval1);
        popreg(sALT);
        ob_add();
        /* adjust the "value" structure and find the referenced array */
        lval1->ident=iREFARRAY;
        lval1->sym=finddepend(sym);
        assert(lval1->sym!=NULL);
        assert(lval1->sym->dim.array.level==sym->dim.array.level-1);
        cursym=lval1->sym;
        /* try to parse subsequent array indices */
        lvalue=FALSE;   /* for now, a iREFARRAY is no lvalue */
        goto restart;
      } /* if */
      assert(sym->dim.array.level==0);
      /* set type to fetch... INDIRECTLY */
      lval1->ident= (char)((close==']' || close==tSYMLABEL) ? iARRAYCELL : iARRAYCHAR);
      /* if the array index is a named index, get the tag from the
       * field and get the size of the field too; otherwise, the
       * tag is the one from the array symbol
       */
      if (lval2.ident==iCONSTEXPR && sym->dim.array.names!=NULL) {
        lval1->tag=lval2.tag;
        assert(cval!=NULL && cval->next!=NULL);
        lval1->constval=cval->next->value - cval->value;
        if (lval1->constval>1)
          lval1->ispacked=lval2.ispacked;   /* copy packed/unpacked status */
        if (lval1->constval>1 && (matchtoken('[') || matchtoken('{'))) {
          /* an array indexed with symbolic name field may be considered a sub-array */
          lexpush();
          lvalue=FALSE;   /* for now, a iREFARRAY is no lvalue */
          lval1->ident=iREFARRAY;
          /* initialize a dummy symbol, which is a copy of the current symbol,
           * but with an adjusted index tag
           */
          assert(sym!=NULL);
          dummysymbol=*sym;
          dummysymbol.dim.array.length=lval1->constval;
          dummysymbol.dim.array.names=NULL; /* disallow symbolic indices in the sub-array */
          if (lval2.ispacked)               /* copy packed/unpacked flag */
            dummysymbol.usage |= uPACKED;
          else
            dummysymbol.usage &= ~uPACKED;
          cursym=&dummysymbol;
          /* recurse */
          goto restart;
        } /* if */
      } else {
        assert(sym!=NULL);
        if (cursym!=&dummysymbol)
          lval1->tag=sym->tag;
        lval1->constval=0;
      } /* if */
      /* a cell in an array is an lvalue, a character in an array is not
       * always a *valid* lvalue */
      return TRUE;
    } else {            /* tok=='(' -> function(...) */
      assert(tok=='(' || tok==tSYMLABEL && !optbrackets);
      if (tok==tSYMLABEL)
        lexpush();      /* analyze the label later, it is a parameter name */
      if (sym==NULL
          || (sym->ident!=iFUNCTN && sym->ident!=iREFFUNC))
      {
        if (sym==NULL && sc_status==statBROWSE) {
          /* could be a "use before declaration"; in that case, create a stub
           * function so that the usage can be marked.
           */
          sym=fetchfunc(lastsymbol,0);
          if (sym==NULL)
            error(103); /* insufficient memory */
          markusage(sym,uREAD);
        } else {
          return error(12);           /* invalid function call */
        } /* if */
      } else if ((sym->usage & uMISSING)!=0) {
        char symname[2*sNAMEMAX+16];  /* allow space for user defined operators */
        funcdisplayname(symname,sym->name);
        error(4,symname);             /* function not defined */
      } /* if */
      callfunction(sym,lval1,(tok=='('));
      return FALSE;             /* result of function call is no lvalue */
    } /* if */
  } /* if */
  if (sym!=NULL && lval1->ident==iFUNCTN) {
    assert(sym->ident==iFUNCTN);
    if (sc_allowproccall) {
      callfunction(sym,lval1,FALSE);
    } else {
      lval1->sym=NULL;
      lval1->ident=iEXPRESSION;
      lval1->constval=0;
      lval1->tag=0;
      error(76);                /* invalid function call, or syntax error */
    } /* if */
    return FALSE;
  } /* if */
  return lvalue;
}

/*  primary
 *
 *  Returns 1 if the operand is an lvalue (everything except arrays, functions
 *  constants and -of course- errors).
 *  Generates code to fetch the address of arrays. Code for constants is
 *  already generated by constant().
 *  This routine first clears the entire lval array (all fields are set to 0).
 *
 *  Global references: sc_intest  (may be altered, but restored upon termination)
 */
static int primary(value *lval,int *symtok)
{
  char *st;
  cell val;
  symbol *sym;

  assert(lval!=NULL);
  assert(symtok!=NULL);
  *symtok=0;
  if (matchtoken('(')){         /* sub-expression - (expression,...) */
    short save_intest=sc_intest;
    short save_allowtags=sc_allowtags;
    int lvalue;

    sc_intest=FALSE;            /* no longer in "test" expression */
    sc_allowtags=TRUE;          /* allow tagnames to be used in parenthesized expressions */
    sc_allowproccall=FALSE;
    do
      lvalue=hier14(lval);
    while (matchtoken(','));
    needtoken(')');
    lexclr(FALSE);              /* clear lex() push-back, it should have been
                                 * cleared already by needtoken() */
    sc_allowtags=save_allowtags;
    sc_intest=save_intest;
    return lvalue;
  } /* if */

  clear_value(lval);    /* clear lval */
  *symtok=lex(&val,&st);
  if (*symtok==tSYMBOL) {
    /* lastsymbol is char[sNAMEMAX+1], lex() should have truncated any symbol
     * to sNAMEMAX significant characters */
    assert(strlen(st)<sizeof lastsymbol);
    strcpy(lastsymbol,st);
  } /* if */
  if (*symtok==tSYMBOL && !findconst(st)) {
    /* first look for a local variable */
    if ((sym=findloc(st))!=0) {
      if (sym->ident==iLABEL) {
        error(29);          /* expression error, assumed 0 */
        ldconst(0,sPRI);    /* load 0 */
        return FALSE;       /* return 0 for labels (expression error) */
      } /* if */
      lval->sym=sym;
      lval->ident=sym->ident;
      lval->tag=sym->tag;
      if (sym->ident==iARRAY || sym->ident==iREFARRAY) {
        address(sym,sPRI);  /* get starting address in primary register */
        return FALSE;       /* return 0 for array (not lvalue) */
      } else {
        return TRUE;        /* return 1 if lvalue (not label or array) */
      } /* if */
    } /* if */
    /* now try a global variable */
    if ((sym=findglb(st,sSTATEVAR))!=NULL) {
      if (sym->ident==iFUNCTN || sym->ident==iREFFUNC) {
        /* if the function is only in the table because it was inserted as a
         * stub in the first pass (i.e. it was "used" but never declared or
         * implemented, issue an error
         */
        if ((sym->usage & uPROTOTYPED)==0) {
          error_suggest(17,st,iVARIABLE); /* undefined symbol */
          if (!matchtoken('('))
            return FALSE; /* undefined symbol, no indication that this is a funtion call */
          lexpush();      /* restore '(' */
        } /* if */
      } else {
        if ((sym->usage & uDEFINE)==0)
          error_suggest(17,st,iVARIABLE); /* undefined symbol */
        lval->sym=sym;
        lval->ident=sym->ident;
        lval->tag=sym->tag;
        if (sym->ident==iARRAY || sym->ident==iREFARRAY) {
          address(sym,sPRI);    /* get starting address in primary register */
          return FALSE;         /* return 0 for array (not lvalue) */
        } else {
          return TRUE;          /* return 1 if lvalue (not function or array) */
        } /* if */
      } /* if */
    } else {
      char symbolname[sNAMEMAX+1];
      assert(strlen(st)<sizearray(symbolname));
      strcpy(symbolname,st);  /* copy symbol name, because lexclr() removes it */
      if (!sc_allowproccall || isbinaryop(lexpeek()) || sc_status!=statBROWSE) {
        lexclr(FALSE);
        return error_suggest(17,symbolname,iVARIABLE);  /* undefined symbol */
      } /* if */
      /* an unknown symbol, but used in a way compatible with the "procedure
       * call" syntax. So assume that the symbol refers to a function.
       */
      sym=fetchfunc(symbolname,0);
      if (sym==NULL)
        error(103);     /* insufficient memory */
    } /* if */
    assert(sym!=NULL);
    assert(sym->ident==iFUNCTN || sym->ident==iREFFUNC);
    lval->sym=sym;
    lval->ident=sym->ident;
    lval->tag=sym->tag;
    return FALSE;       /* return 0 for function (not an lvalue) */
  } /* if */
  lexpush();            /* push the token, it is analyzed by constant() */
  if (constant(lval)==0) {
    error(29);          /* expression error, assumed 0 */
    ldconst(0,sPRI);    /* load 0 */
	/* Normally, gobble up unrecognized symbols, but make an exception for '}',
	 *  because it closes compound statements.
	 */
    if (*symtok=='}')
      lexpush();
  } /* if */
  return FALSE;         /* return 0 for constants (or errors) */
}

static void clear_value(value *lval)
{
  lval->sym=NULL;
  lval->constval=0L;
  lval->tag=0;
  lval->ident=0;
  lval->boolresult=FALSE;
  lval->ispacked=FALSE;
  /* do not clear lval->arrayidx, it is preset in hier14() */
}

static void setdefarray(cell *string,cell size,cell array_sz,cell *dataaddr,int fconst)
{
  /* The routine must copy the default array data onto the heap, as to avoid
   * that a function can change the default value. An optimization is that
   * the default array data is "dumped" into the data segment only once (on the
   * first use).
   */
  assert(string!=NULL);
  assert(size>0);
  /* check whether to dump the default array */
  assert(dataaddr!=NULL);
  if (sc_status==statWRITE && *dataaddr<0) {
    int i;
    *dataaddr=(litidx+glb_declared)*pc_cellsize;
    for (i=0; i<size; i++)
      litadd(*string++);
  } /* if */

  /* if the function is known not to modify the array (meaning that it also
   * does not modify the default value), directly pass the address of the
   * array in the data segment.
   */
  if (fconst) {
    ldconst(*dataaddr,sPRI);
  } else {
    /* Generate the code:
     *  CONST.pri dataaddr                ;address of the default array data
     *  HEAP      array_sz*pc_cellsize    ;heap address in ALT
     *  MOVS      size*pc_cellsize        ;copy data from PRI to ALT
     *  MOVE.PRI                          ;PRI = address on the heap
     */
    ldconst(*dataaddr,sPRI);
    /* "array_sz" is the size of the argument (the value between the brackets
     * in the declaration), "size" is the size of the default array data.
     */
    assert(array_sz>=size);
    modheap((int)array_sz*pc_cellsize);
    /* the amount of data stored on the heap is kept in a local variable in
     * callfunction(); it is therefore not necessary to adjust decl_heap
     */
    /* ??? should perhaps fill with zeros first */
    memcopy(size*pc_cellsize);
    swapregs();
  } /* if */
}

static int findnamedarg(const arginfo *arg,const char *name, char *closestarg)
{
  int i;
  int dist,closestdist=INT_MAX;

  if (closestarg!=NULL)
    *closestarg='\0';
  for (i=0; arg[i].ident!=0 && arg[i].ident!=iVARARGS; i++) {
    if (strcmp(arg[i].name,name)==0)
      return i;
    if (closestarg!=NULL) {
      dist=levenshtein_distance(arg[i].name,name);
      if (dist<closestdist && dist<=MAX_EDIT_DIST) {
        strcpy(closestarg,arg[i].name);
        closestdist=dist;
      } /* if */
    } /* if */
  } /* for */
  return -1;
}

static int checktag(int tags[],int numtags,int exprtag)
{
  int i;

  assert(tags!=0);
  assert(numtags>0);
  for (i=0; i<numtags; i++)
    if (matchtag(tags[i],exprtag,TRUE))
      return TRUE;    /* matching tag */
  return FALSE;       /* no tag matched */
}

enum {
  ARG_UNHANDLED,
  ARG_IGNORED,
  ARG_DONE,
};

/*  callfunction
 *
 *  Generates code to call a function. This routine handles default arguments
 *  and positional as well as named parameters.
 */
static void callfunction(symbol *sym,value *lval_result,int matchparanthesis)
{
static long nest_stkusage=0L;
static int nesting=0;
  int locheap;
  int close,lvalue;
  int argpos;       /* index in the output stream (argpos==nargs if positional parameters) */
  int argidx=0;     /* index in "arginfo" list */
  int nargs=0;      /* number of arguments */
  int heapalloc=0;
  int namedparams=FALSE;
  value lval = {0};
  arginfo *arg;
  char arglist[sMAXARGS];
  char closestname[sNAMEMAX+1];
  constvalue arrayszlst = { NULL, "", 0, 0};/* array size list starts empty */
  constvalue taglst = { NULL, "", 0, 0};    /* tag list starts empty */
  symbol *symret;
  cell lexval;
  char *lexstr;
  int reloc;

  assert(sym!=NULL);
  lval_result->ident=iEXPRESSION; /* preset, may be changed later */
  lval_result->constval=0;
  lval_result->tag=sym->tag;
  /* check whether this is a function that returns an array */
  symret=finddepend(sym);
  assert(symret==NULL || symret->ident==iREFARRAY);
  if (symret!=NULL) {
    int retsize;
    /* allocate space on the heap for the array, and pass the pointer to the
     * reserved memory block as a hidden parameter
     */
    retsize=(int)array_totalsize(symret);
    assert(retsize>0);
    modheap(retsize*pc_cellsize); /* address is in ALT */
    pushreg(sALT);                /* pass ALT as the last (hidden) parameter */
    if ((sym->usage & uNATIVE)!=0) {
      /* for a native function that returns an array, the address passed to the
         native function must be passed as relocated, but for an assignment after
         returning from the function, the non-relocated value is needed -> push
         both (and drop one extra item from the stack before returning) */
      swapregs();
      pushreloc();
    }
    decl_heap+=retsize;
    /* also mark the ident of the result as "array" */
    lval_result->ident=iREFARRAY;
    lval_result->sym=symret;
  } /* if */
  locheap=decl_heap;

  nesting++;
  assert(nest_stkusage>=0);
  #if !defined NDEBUG
    if (nesting==1)
      assert(nest_stkusage==0);
  #endif
  sc_allowproccall=FALSE;       /* parameters may not use procedure call syntax */

  if ((sym->flags & flgDEPRECATED)!=0) {
    char *ptr= (sym->documentation!=NULL) ? sym->documentation : "";
    error(234,sym->name,ptr);   /* deprecated (probably a native function) */
  } /* if */

  /* run through the arguments */
  arg=sym->dim.arglist;
  assert(arg!=NULL);
  stgmark(sSTARTREORDER);
  memset(arglist,ARG_UNHANDLED,sizeof arglist);
  if (matchparanthesis) {
    /* Opening parenthesis was already parsed, if parenthesis brace follows, this
     * call passes no parameters.
     */
    close=matchtoken(')');
  } else {
    /* When we find an end-of-line here, it may be a function call passing
     * no parameters, or it may be that the first parameter is on a line
     * below. But as a parameter can be anything, this is difficult to check.
     * The only simple check that we have is the use of "named parameters".
     */
    close=matchtoken(tTERM);
    if (close) {
      close=!matchtoken(tSYMLABEL);
      if (!close)
        lexpush();                /* reset the '.' */
    } else {
      /* There is no end-of line, but a closing brace also terminates the
       * argument list
       */
      close=matchtoken('}');
      if (close)
        lexpush();                /* restore the '}', to be analyzed later */
    } /* if */
  } /* if */
  if (!close) {
    do {
      if (matchtoken(tSYMLABEL)) {
        namedparams=TRUE;
        tokeninfo(&lexval,&lexstr);
        argpos=findnamedarg(arg,lexstr,closestname);
        if (argpos<0) {
          if (closestname[0]!='\0')
            error(makelong(17,1),lexstr,closestname);
          else
            error(17,lexstr);     /* undefined symbol (meaning "undefined argument") */
          break;                  /* exit loop, argpos is invalid */
        } /* if */
        needtoken('=');
        argidx=argpos;
      } else {
        if (namedparams)
          error(44);   /* positional parameters must precede named parameters */
        argpos=nargs;
      } /* if */
      /* the number of arguments this was already checked at the declaration
       * of the function; check it again for functions with a variable
       * argument list
       */
      if (argpos>=sMAXARGS)
        error(45);                /* too many function arguments */
      stgmark((char)(sEXPRSTART+argpos));/* mark beginning of new expression in stage */
      if (argpos<sMAXARGS && arglist[argpos]!=ARG_UNHANDLED)
        error(58);                /* argument already set */
      if (matchtoken('_')) {
        arglist[argpos]=ARG_IGNORED;  /* flag argument as "present, but ignored" */
        if (arg[argidx].ident==0 || arg[argidx].ident==iVARARGS) {
          error(202);             /* argument count mismatch */
        } else if (!arg[argidx].hasdefault) {
          error(34,nargs+1);      /* argument has no default value */
        } /* if */
        if (arg[argidx].ident!=0 && arg[argidx].ident!=iVARARGS)
          argidx++;
        /* The rest of the code to handle default values is at the bottom
         * of this routine where default values for unspecified parameters
         * are (also) handled. Note that above, the argument is flagged as
         * ARG_IGNORED.
         */
      } else {
        arglist[argpos]=ARG_DONE; /* flag argument as "present" */
        lvalue=hier14(&lval);
        assert(sc_status==statBROWSE || arg[argidx].ident== 0 || arg[argidx].tags!=NULL);
        reloc=FALSE;
        switch (arg[argidx].ident) {
        case 0:
          error(202);             /* argument count mismatch */
          break;
        case iVARARGS:
          /* always pass by reference */
          if ((sym->usage & uNATIVE)!=0)
            reloc=TRUE;
          if (lval.ident==iVARIABLE || lval.ident==iREFERENCE) {
            assert(lval.sym!=NULL);
            if ((lval.sym->usage & uCONST)!=0 && (arg[argidx].usage & uCONST)==0) {
              /* treat a "const" variable passed to a function with a non-const
               * "variable argument list" as a constant here */
              if (!lvalue) {
                error(22);        /* need lvalue */
              } else {
                rvalue(&lval);    /* get value in PRI */
                setheap_pri();    /* address of the value on the heap in PRI */
                heapalloc++;
                nest_stkusage++;
              } /* if */
            } else if (lvalue) {
              address(lval.sym,sPRI);
            } else {
              setheap_pri();      /* address of the value on the heap in PRI */
              heapalloc++;
              nest_stkusage++;
            } /* if */
          } else if (lval.ident==iCONSTEXPR || lval.ident==iEXPRESSION
                     || lval.ident==iARRAYCHAR)
          {
            /* fetch value if needed */
            if (lval.ident==iARRAYCHAR)
              rvalue(&lval);
            /* allocate a cell on the heap and store the
             * value (already in PRI) there */
            setheap_pri();        /* address of the value on the heap in PRI */
            heapalloc++;
            nest_stkusage++;
          } /* if */
          /* ??? handle const array passed by reference */
          /* otherwise, the address is already in PRI */
          if (lval.sym!=NULL)
            markusage(lval.sym,uWRITTEN);
          if (!checktag(arg[argidx].tags,arg[argidx].numtags,lval.tag))
            error(213);
          if (lval.tag!=0)
            append_constval(&taglst,arg[argidx].name,lval.tag,0);
          break;
        case iVARIABLE:
          if (lval.ident==iLABEL || lval.ident==iFUNCTN || lval.ident==iREFFUNC
              || lval.ident==iARRAY || lval.ident==iREFARRAY)
            error(35,argidx+1);   /* argument type mismatch */
          if (lvalue)
            rvalue(&lval);        /* get value (direct or indirect) */
          /* otherwise, the expression result is already in PRI */
          assert(arg[argidx].numtags>0);
          check_userop(NULL,lval.tag,arg[argidx].tags[0],2,NULL,&lval.tag);
          if (!checktag(arg[argidx].tags,arg[argidx].numtags,lval.tag))
            error(213);
          if (lval.tag!=0)
            append_constval(&taglst,arg[argidx].name,lval.tag,0);
          argidx++;               /* argument done */
          break;
        case iREFERENCE:
          if ((sym->usage & uNATIVE)!=0)
            reloc=TRUE;
          if (!lvalue || lval.ident==iARRAYCHAR)
            error(35,argidx+1);   /* argument type mismatch */
          if (lval.sym!=NULL && (lval.sym->usage & uCONST)!=0 && (arg[argidx].usage & uCONST)==0)
            error(35,argidx+1);   /* argument type mismatch */
          if (lval.ident==iVARIABLE || lval.ident==iREFERENCE) {
            if (lvalue) {
              assert(lval.sym!=NULL);
              address(lval.sym,sPRI);
            } else {
              setheap_pri();      /* address of the value on the heap in PRI */
              heapalloc++;
              nest_stkusage++;
            } /* if */
          } /* if */
          /* otherwise, the address is already in PRI */
          if (!checktag(arg[argidx].tags,arg[argidx].numtags,lval.tag))
            error(213);
          if (lval.tag!=0)
            append_constval(&taglst,arg[argidx].name,lval.tag,0);
          argidx++;               /* argument done */
          if (lval.sym!=NULL)
            markusage(lval.sym,uWRITTEN);
          break;
        case iREFARRAY:
          if ((sym->usage & uNATIVE)!=0)
            reloc=TRUE;
          if (lval.ident!=iARRAY && lval.ident!=iREFARRAY
              && lval.ident!=iARRAYCELL)
          {
            error(35,argidx+1);   /* argument type mismatch */
            break;
          } /* if */
          if (lval.sym!=NULL && (lval.sym->usage & uCONST)!=0 && (arg[argidx].usage & uCONST)==0)
            error(35,argidx+1); /* argument type mismatch */
          /* Verify that the dimensions and the sizes match with those in arg[argidx].
           * A literal array always has a single dimension.
           * An iARRAYCELL parameter is also assumed to have a single dimension,
           * but its size may be >1 in case of a pseudo-array.
           */
          if (lval.sym==NULL || lval.ident==iARRAYCELL) {
            if (arg[argidx].numdim!=1) {
              error(48);        /* array dimensions must match */
            } else if (arg[argidx].dim[0]!=0) {
              assert(arg[argidx].dim[0]>0);
              if (lval.ident==iARRAYCELL) {
                if (lval.constval==0 || arg[argidx].dim[0]!=lval.constval)
                  error(47);        /* array sizes must match */
              } else {
                assert(lval.constval!=0); /* literal array must have a size */
                /* A literal array must have exactly the same size as the
                 * function argument; a literal string may be smaller than
                 * the function argument.
                 */
                if (lval.constval>0 && arg[argidx].dim[0]!=lval.constval
                    || lval.constval<0 && arg[argidx].dim[0] < -lval.constval)
                  error(47);      /* array sizes must match */
              } /* if */
            } /* if */
            if ((arg[argidx].usage & uPACKED)!=0 && !lval.ispacked)
              error(229);         /* formal argument is packed, real argument is unpacked */
            if (lval.ident!=iARRAYCELL || lval.constval>0) {
              /* save array size, for default values with uSIZEOF flag */
              cell array_sz=lval.constval;
              assert(array_sz!=0);/* literal array must have a size */
              if (array_sz<0)
                array_sz= -array_sz;
              append_constval(&arrayszlst,arg[argidx].name,array_sz,0);
            } /* if */
          } else {
            symbol *rsym=lval.sym;
            short level=0;
            assert(rsym!=NULL);
            if (rsym->dim.array.level+1!=arg[argidx].numdim)
              error(48);          /* array dimensions must match */
            /* the lengths for all dimensions must match, unless the dimension
             * length was defined at zero (which means "undefined")
             */
            while (rsym->dim.array.level>0) {
              assert(level<sDIMEN_MAX);
              if (arg[argidx].dim[level]!=0 && rsym->dim.array.length!=arg[argidx].dim[level])
                error(47);        /* array sizes must match */
              if (!compare_consttable(arg[argidx].dimnames[level],rsym->dim.array.names))
                error(47);        /* array definitions must match */
              append_constval(&arrayszlst,arg[argidx].name,rsym->dim.array.length,level);
              rsym=finddepend(rsym);
              assert(rsym!=NULL);
              level++;
            } /* if */
            /* the last dimension is checked too, again, unless it is zero */
            assert(level<sDIMEN_MAX);
            assert(rsym!=NULL);
            if (arg[argidx].dim[level]!=0 && rsym->dim.array.length!=arg[argidx].dim[level])
              error(47);          /* array sizes must match */
            if (!compare_consttable(arg[argidx].dimnames[level],rsym->dim.array.names))
              error(47);        /* array definitions must match */
            append_constval(&arrayszlst,arg[argidx].name,rsym->dim.array.length,level);
          } /* if */
          /* address already in PRI */
          if (!checktag(arg[argidx].tags,arg[argidx].numtags,lval.tag))
            error(213);
          if (lval.tag!=0)
            append_constval(&taglst,arg[argidx].name,lval.tag,0);
          // ??? set uWRITTEN?
          argidx++;               /* argument done */
          break;
        default:
          assert(0);
        } /* switch */
        if (reloc)
          pushreloc();            /* store argument on the stack, mark for run-time relocation */
        else
          pushreg(sPRI);          /* store the argument on the stack */
        markexpr(sPARM,NULL,0);   /* mark the end of a sub-expression */
        nest_stkusage++;
      } /* if */
      assert(arglist[argpos]!=ARG_UNHANDLED);
      nargs++;
      if (matchparanthesis) {
        close=matchtoken(')');
        if (!close)               /* if not paranthese... */
          if (!needtoken(','))    /* ...should be comma... */
            break;                /* ...but abort loop if neither */
      } else {
        close=!matchtoken(',');
        if (close) {              /* if not comma... */
          if (needtoken(tTERM)==1)/* ...must be end of statement */
            lexpush();            /* push again, because end of statement is analised later */
        } /* if */
      } /* if */
    } while (!close && freading && !matchtoken(tENDEXPR)); /* do */
  } /* if */
  /* check remaining function arguments (they may have default values) */
  for (argidx=0; arg[argidx].ident!=0 && arg[argidx].ident!=iVARARGS; argidx++) {
    if (arglist[argidx]==ARG_DONE)
      continue;                 /* already seen and handled this argument */
    /* in this first stage, we also skip the arguments with uSIZEOF and uTAGOF;
     * these are handled last
     */
    if ((arg[argidx].hasdefault & uSIZEOF)!=0 || (arg[argidx].hasdefault & uTAGOF)!=0) {
      assert(arg[argidx].ident==iVARIABLE);
      continue;
    } /* if */
    stgmark((char)(sEXPRSTART+argidx));/* mark beginning of new expression in stage */
    if (arg[argidx].hasdefault) {
      reloc=FALSE;
      if (arg[argidx].ident==iREFARRAY) {
        if ((sym->usage & uNATIVE)!=0)
          reloc=TRUE;
        setdefarray(arg[argidx].defvalue.array.data,
                    arg[argidx].defvalue.array.size,
                    arg[argidx].defvalue.array.arraysize,
                    &arg[argidx].defvalue.array.addr,
                    (arg[argidx].usage & uCONST)!=0);
        if ((arg[argidx].usage & uCONST)==0) {
          heapalloc+=arg[argidx].defvalue.array.arraysize;
          nest_stkusage+=arg[argidx].defvalue.array.arraysize;
        } /* if */
        /* keep the lengths of all dimensions of a multi-dimensional default array */
        assert(arg[argidx].numdim>0);
        if (arg[argidx].numdim==1) {
          append_constval(&arrayszlst,arg[argidx].name,arg[argidx].defvalue.array.arraysize,0);
        } else {
          short level;
          for (level=0; level<arg[argidx].numdim; level++) {
            assert(level<sDIMEN_MAX);
            append_constval(&arrayszlst,arg[argidx].name,arg[argidx].dim[level],level);
          } /* for */
        } /* if */
      } else if (arg[argidx].ident==iREFERENCE) {
        if ((sym->usage & uNATIVE)!=0)
          reloc=TRUE;
        setheap(arg[argidx].defvalue.val);
        /* address of the value on the heap in PRI */
        heapalloc++;
        nest_stkusage++;
      } else {
        int dummytag=arg[argidx].tags[0];
        ldconst(arg[argidx].defvalue.val,sPRI);
        assert(arg[argidx].numtags>0);
        check_userop(NULL,arg[argidx].defvalue_tag,arg[argidx].tags[0],2,NULL,&dummytag);
        assert(dummytag==arg[argidx].tags[0]);
      } /* if */
      if (reloc)
        pushreloc();            /* store argument on the stack, mark for run-time relocation */
      else
        pushreg(sPRI);          /* store the argument on the stack */
      markexpr(sPARM,NULL,0);   /* mark the end of a sub-expression */
      nest_stkusage++;
      if (arglist[argidx]==ARG_UNHANDLED)
        nargs++;
      arglist[argidx]=ARG_DONE;
    } else {
      error(202,argidx);        /* argument count mismatch */
      arglist[argidx]=ARG_DONE; /* mark as done, so we do not process this parameter again */
    } /* if */
  } /* for */
  /* now a second loop to catch the arguments with default values that are
   * the "sizeof" or "tagof" of other arguments
   */
  for (argidx=0; arg[argidx].ident!=0 && arg[argidx].ident!=iVARARGS; argidx++) {
    constvalue *asz;
    cell array_sz;
    if (arglist[argidx]==ARG_DONE)
      continue;                 /* already seen and handled this argument */
    stgmark((char)(sEXPRSTART+argidx));/* mark beginning of new expression in stage */
    assert(arg[argidx].ident==iVARIABLE);           /* if "sizeof", must be single cell */
    /* if unseen, must be "sizeof" or "tagof" */
    assert((arg[argidx].hasdefault & uSIZEOF)!=0 || (arg[argidx].hasdefault & uTAGOF)!=0);
    if ((arg[argidx].hasdefault & uSIZEOF)!=0) {
      /* find the argument; if it isn't found, the argument's default value
       * was a "sizeof" of a non-array (a warning for this was already given
       * when declaring the function)
       */
      asz=find_constval(&arrayszlst,arg[argidx].defvalue.size.symname,
                        arg[argidx].defvalue.size.level);
      if (asz!=NULL) {
        array_sz=asz->value;
        if (array_sz==0)
          error(224,arg[argidx].name);    /* indeterminate array size in "sizeof" expression */
      } else {
        array_sz=1;
      } /* if */
    } else {
      asz=find_constval(&taglst,arg[argidx].defvalue.size.symname,
                        arg[argidx].defvalue.size.level);
      if (asz != NULL) {
        exporttag((int)asz->value);
        array_sz=asz->value | PUBLICTAG;  /* must be set, because it just was exported */
      } else {
        array_sz=0;
      } /* if */
    } /* if */
    ldconst(array_sz,sPRI);
    pushreg(sPRI);              /* store the function argument on the stack (never relocated) */
    markexpr(sPARM,NULL,0);
    nest_stkusage++;
    if (arglist[argidx]==ARG_UNHANDLED)
      nargs++;
    arglist[argidx]=ARG_DONE;
  } /* for */
  stgmark(sENDREORDER);         /* mark end of reversed evaluation */
  pushval((cell)nargs*pc_cellsize);
  nest_stkusage++;
  ffcall(sym,NULL,nargs);
  if (sc_status!=statSKIP)
    markusage(sym,uREAD);       /* do not mark as "used" when this call itself is skipped */
  if ((sym->usage & uNATIVE)!=0 &&sym->x.lib!=NULL)
    sym->x.lib->value += 1;     /* increment "usage count" of the library */
  modheap(-heapalloc*pc_cellsize);
  if (symret!=NULL) {
    if ((sym->usage & uNATIVE)!=0)
      modstk(1*pc_cellsize);    /* remove relocated hidden parameter */
    popreg(sPRI);               /* pop hidden parameter as function result */
  }
  pc_sideeffect=TRUE;           /* assume functions carry out a side-effect */
  delete_consttable(&arrayszlst);     /* clear list of array sizes */
  delete_consttable(&taglst);   /* clear list of parameter tags */

  /* maintain max. amount of memory used
   * "curfunc" should always be valid, since expression statements (like a
   * function call) may not occur outside functions; in the case of syntax
   * errors, however, the compiler may arrive through this function
   */
  if (curfunc!=NULL) {
    long totalsize;
    totalsize=(long)declared+decl_heap+1; /* local variables & return value size,
                                           * +1 for PROC opcode */
    if (lval_result->ident==iREFARRAY)
      totalsize++;                    /* add hidden parameter (on the stack) */
    if ((sym->usage & uNATIVE)==0)
      totalsize++;                    /* add "call" opcode */
    totalsize+=nest_stkusage;
    assert(curfunc!=NULL);
    if (curfunc->x.stacksize<totalsize)
      curfunc->x.stacksize=totalsize;
  } /* if */
  nest_stkusage-=nargs+heapalloc+1;   /* stack/heap space, +1 for argcount param */
  /* if there is a syntax error in the script, the stack calculation is
   * probably incorrect; but we may not allow it to drop below zero
   */
  if (nest_stkusage<0)
    nest_stkusage=0;

  /* scrap any arrays left on the heap, with the exception of the array that
   * this function has as a result (in other words, scrap all arrays on the
   * heap that caused by expressions in the function arguments)
   */
  assert(decl_heap>=locheap);
  modheap((locheap-decl_heap)*pc_cellsize); /* remove heap space, so negative delta */
  decl_heap=locheap;
  nesting--;
}

/* skiptotoken
 * skip (ignore) all text up to (and including) the given token; returns
 * 0 if the end of the file is reached before finding the token
 */
static int skiptotoken(int token)
{
  while (freading) {
    cell val;
    char *str;
    int tok=lex(&val,&str);
    if (tok==token)
      break;
  } /* while */
  return freading;
}

/*  dbltest
 *
 *  Returns the cell size in bytes if lval1 is an array and lval2 is not an array
 *  and the operation is addition or subtraction. This is to align cell indices
 *  byte offsets. In all other cases, the function returns 1. The result of this
 *  function can therefore be used to multiple the array index.
 */
static int dbltest(const void (*oper)(),value *lval1,value *lval2)
{
  if ((oper!=ob_add) && (oper!=ob_sub))
    return 1;
  if (lval1->ident!=iARRAY)
    return 1;
  if (lval2->ident==iARRAY)
    return 1;
  return pc_cellsize;
}

/*  commutative
 *
 *  Test whether an operator is commutative, i.e. x oper y == y oper x.
 *  Commutative operators are: +  (addition)
 *                             *  (multiplication)
 *                             == (equality)
 *                             != (inequality)
 *                             &  (bitwise and)
 *                             ^  (bitwise xor)
 *                             |  (bitwise or)
 *
 *  If in an expression, code for the left operand has been generated and
 *  the right operand is a constant and the operator is commutative, the
 *  precautionary "push" of the primary register is scrapped and the constant
 *  is read into the secondary register immediately.
 */
static int commutative(const void (*oper)())
{
  return oper==ob_add || oper==os_mult
         || oper==ob_eq || oper==ob_ne
         || oper==ob_and || oper==ob_xor || oper==ob_or;
}

/*  constant
 *
 *  Generates code to fetch a number, a literal character (which is returned
 *  by lex() as a number as well) or a literal string (lex() stores the
 *  strings in the literal queue). If the operand was a number, it is stored
 *  in lval->constval.
 *
 *  The function returns 1 if the token was a constant or a string, 0
 *  otherwise.
 */
static int constant(value *lval)
{
  int tok,index,ident;
  cell val,item,cidx;
  char *st;
  symbol *sym;

  tok=lex(&val,&st);
  if (tok==tSYMBOL && (sym=findconst(st))!=0) {
    lval->constval=sym->addr;
    ldconst(lval->constval,sPRI);
    lval->ident=iCONSTEXPR;
    lval->tag=sym->tag;
    lval->sym=sym;
    markusage(sym,uREAD);
  } else if (tok==tNUMBER) {
    lval->constval=val;
    ldconst(lval->constval,sPRI);
    lval->ident=iCONSTEXPR;
    lastsymbol[0]='\0';
  } else if (tok==tRATIONAL) {
    lval->constval=val;
    ldconst(lval->constval,sPRI);
    lval->ident=iCONSTEXPR;
    lval->tag=sc_rationaltag;
    lastsymbol[0]='\0';
  } else if (tok==tSTRING || tok==tPACKSTRING) {
    /* lex() stores starting index of string in the literal table in 'val' */
    ldconst((val+glb_declared)*pc_cellsize,sPRI);
    lval->ident=iARRAY;         /* pretend this is a global array */
    lval->constval=val-litidx;  /* constval == the negative value of the
                                 * size of the literal array; using a negative
                                 * value distinguishes between literal arrays
                                 * and literal strings (this was done for
                                 * array assignment). */
    lval->ispacked= (tok==tPACKSTRING);
    lastsymbol[0]='\0';
    //??? add this to the list of "constant literal arrays"
  } else if (tok=='{' || tok=='[') {
    int match,packcount,tag,lasttag=-1;
    cell packitem;
    match=(tok=='{') ? '}' : ']';
    val=litidx;
    packitem=0;
    packcount=0;
    do {
      /* cannot call constexpr() here, because "staging" is already turned
       * on at this point */
      assert(staging);
      stgget(&index,&cidx);     /* mark position in code generator */
      ident=expression(&item,&tag,NULL,FALSE);
      stgdel(index,cidx);       /* scratch generated code */
      if (ident!=iCONSTEXPR)
        error(8);               /* must be constant expression */
      if (lasttag<0)
        lasttag=tag;
      else if (!matchtag(lasttag,tag,FALSE))
        error(213);             /* tagname mismatch */
      if (match=='}') {
        if ((ucell)item>=(ucell)(1 << sCHARBITS))
          error(43,(long)item); /* constant exceeds range */
        assert(packcount<pc_cellsize);
        packcount++;
        packitem|=(item & 0xff) << ((pc_cellsize-packcount)*sCHARBITS);
        if (packcount==pc_cellsize) {
          litadd(packitem);     /* store collected values in literal table */
          packitem=0;
          packcount=0;
        } /* if */
      } else {
        litadd(item);           /* store expression result in literal table */
      } /* if */
    } while (matchtoken(','));
    if (packcount!=0 && match=='}')
      litadd(packitem);         /* store final collected values */
    if (!needtoken(match))
      lexclr(FALSE);
    ldconst((val+glb_declared)*pc_cellsize,sPRI);
    lval->ident=iARRAY;         /* pretend this is a global array */
    lval->constval=litidx-val;  /* constval == the size of the literal array */
    lval->ispacked= (match=='}'); /* flag packed array */
    lastsymbol[0]='\0';
    //??? add this to the list of "constant literal arrays"
  } else {
    return FALSE;               /* no, it cannot be interpreted as a constant */
  } /* if */
  return TRUE;                  /* yes, it was a constant value */
}
