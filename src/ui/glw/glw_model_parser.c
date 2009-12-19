/*
 *  GL Widgets, model loader, parser
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include "glw_model.h"
#include <stdio.h>

static int parse_block(token_t *first, errorinfo_t *ei, token_type_t term);
static int parse_array(token_t *first, errorinfo_t *ei);


typedef struct tokenqueue {
  token_t *head, *tail;
} tokenqueue_t;

/**
 *
 */
static token_t *
tokenqueue_enqueue(tokenqueue_t *q, token_t *t, token_t *f)
{
  token_t *r = t->next;
  t->next = NULL;

  if(q->head == NULL) {
    q->head = q->tail = t;
  } else {
    q->tail->next = t;
    q->tail = t;
  }

  if(f != NULL && !(f->t_num_args & 1))
    f->t_num_args++;

  return r;
}

/**
 *
 */
static token_t *
tokenstack_push(token_t **s, token_t *t)
{
  token_t *r = t->next;

  t->next = *s;
  *s = t;
  return r;
}


/**
 *
 */
static token_t *
tokenstack_pop(token_t **s)
{
  token_t *r = *s;

  *s = r->next;
  return r;
}


/**
 *
 */
static const int tokenprecedence[TOKEN_num] = {
  [TOKEN_ASSIGNMENT] = 1,
  [TOKEN_BOOLEAN_OR] = 2,
  [TOKEN_BOOLEAN_AND]= 3,
  [TOKEN_BOOLEAN_XOR]= 4,
  [TOKEN_EQ]         = 5,
  [TOKEN_NEQ]        = 5,
  [TOKEN_ADD]        = 6,
  [TOKEN_SUB]        = 6,
  [TOKEN_MULTIPLY]   = 7,
  [TOKEN_DIVIDE]     = 7,
  [TOKEN_MODULO]     = 7,
  [TOKEN_BLOCK]      = 10,
};


/**
 * Convert an infix expression into an RPN expression
 *
 * Based on: http://en.wikipedia.org/wiki/Shunting_yard_algorithm
 *
 * Modified to keep track of number of arguments passed to a function.
 * This is done by using t_num_args.
 *  If it is even, and a value is to be pushed, it is increased by 1
 *  If we stumble on a ',' (TOKEN_SEPARATOR) we increase it by 1 if
 *  it is odd. If it is even we've stumbled on a syntax error (two
 *  commas in a row)
 *
 *  Finally, the number of arguments is 1 + (num_args / 2)
 */

static int
parse_shunting_yard(token_t *expr, errorinfo_t *ei)
{
  tokenqueue_t outq = {NULL, NULL};
  token_t *stack = NULL;
  token_t *t = expr->child, *x;
  token_t *curfunc = NULL;

  expr->child = NULL;  /* Avoid duplicate free if we bail out */

  while(t != NULL) {

    switch(t->type) {
    case TOKEN_STRING:
    case TOKEN_FLOAT:
    case TOKEN_IDENTIFIER:
    case TOKEN_OBJECT_ATTRIBUTE:
    case TOKEN_ARRAY:
    case TOKEN_BLOCK:
    case TOKEN_PROPERTY_NAME:
    case TOKEN_VOID:
      t = tokenqueue_enqueue(&outq, t, curfunc);
      continue;

    case TOKEN_SEPARATOR:
      while(stack && stack->type != TOKEN_LEFT_PARENTHESIS)
	tokenqueue_enqueue(&outq, tokenstack_pop(&stack), curfunc);
      if(curfunc != NULL) {
	if(!(curfunc->t_num_args & 1)) {
	  glw_model_seterr(ei, t, "Unexpected separator '',''");
 	  goto err;
	}
      }
      curfunc->t_num_args++;
      break;

    case TOKEN_ADD:
    case TOKEN_SUB:
    case TOKEN_MULTIPLY:
    case TOKEN_DIVIDE:
    case TOKEN_MODULO:
    case TOKEN_BOOLEAN_OR:
    case TOKEN_BOOLEAN_AND:
    case TOKEN_BOOLEAN_XOR:
    case TOKEN_ASSIGNMENT:
    case TOKEN_EQ:
    case TOKEN_NEQ:
      while(stack && tokenprecedence[t->type] <= tokenprecedence[stack->type])
	tokenqueue_enqueue(&outq, tokenstack_pop(&stack), NULL);
      /* FALLTHRU */
    case TOKEN_LEFT_PARENTHESIS:
    case TOKEN_BOOLEAN_NOT:
      t = tokenstack_push(&stack, t);
      continue;

    case TOKEN_FUNCTION:
      t->tmp = curfunc;
      curfunc = t;

      t = tokenstack_push(&stack, t);
      continue;

    case TOKEN_RIGHT_PARENTHESIS:
      while(stack && stack->type != TOKEN_LEFT_PARENTHESIS)
	tokenqueue_enqueue(&outq, tokenstack_pop(&stack), curfunc);

      if(stack == NULL) {
	glw_model_seterr(ei, t, "Unbalanced parentheses");
	goto err;
      }
      glw_model_token_free(tokenstack_pop(&stack));

      if(stack && stack->type == TOKEN_FUNCTION) {
	assert(stack == curfunc);

	if(stack->t_num_args && !(stack->t_num_args & 1)) {
	  glw_model_seterr(ei, t, "Unexpected separator '',''");
 	  goto err;
	}

	stack->t_num_args = (1 + stack->t_num_args) / 2;
	curfunc = stack->tmp;
	tokenqueue_enqueue(&outq, tokenstack_pop(&stack), curfunc);
      }
      break;
      
    default:
      glw_model_seterr(ei, t, "Unexpected symbol");
      goto err;
    }
    x = t;
    t = t->next;
    glw_model_token_free(x);
  }

  while(stack) {
    if(stack->type == TOKEN_LEFT_PARENTHESIS || 
       stack->type == TOKEN_RIGHT_PARENTHESIS) {
      glw_model_seterr(ei, stack, "Unbalanced parentheses");
      goto err;
    }
    tokenqueue_enqueue(&outq, tokenstack_pop(&stack), curfunc);
  }

  expr->child = outq.head;
  expr->type = TOKEN_RPN;
  return 0;

 err:

  while(stack != NULL)
    glw_model_token_free(tokenstack_pop(&stack));

  while(outq.head != NULL)
    glw_model_token_free(tokenstack_pop(&outq.head));

  return -1;
}

/**
 *
 */
static int
parse_prep_expression(token_t *expr, errorinfo_t *ei)

{
  token_t *t = expr->child, *t0, *t1, *t2;

  while(t != NULL) {

    t1 = t->next;

    /**
     * Transform [$&]foo.bar.etc into a property chain
     */
    if(t->type == TOKEN_DOLLAR && t1 != NULL && t1->type == TOKEN_IDENTIFIER) {

      t0 = t2 = t;
      
      t0->type = TOKEN_PROPERTY_NAME;
      t0->next = t1->next;
      t0->t_rstring = t1->t_rstring;
      t1->t_rstring = NULL;

      glw_model_token_free(t1);


      t = t0->next;
      
      while(t != NULL && t->type == TOKEN_DOT) {
	t1 = t->next;
	if(t1 == NULL || t1->type != TOKEN_IDENTIFIER) {
	  glw_model_seterr(ei, t1, "Invalid object dereference");
	  return -1;
	}

	t0->next = t1->next;
	t1->next = NULL;

	glw_model_token_free(t);

	t2->child = t1;
	t2 = t1;
	t = t0->next;
      }
      continue;
    }

    /**
     * Transform '.name' into just 'name' and set its type to
     * object attribute
     */
    if(t->type == TOKEN_DOT) {
      if(t1 == NULL || t1->type != TOKEN_IDENTIFIER) {
	glw_model_seterr(ei, t, "Invalid object attribute reference");
	return -1;
      }
      
      t->t_rstring = t1->t_rstring;
      t1->t_rstring = NULL;

      if(glw_model_attrib_resolve(t))
	return glw_model_seterr(ei, t, "Unknown attribute: %s",
				rstr_get(t->t_rstring));

      t->next = t1->next;
      t = t1->next;
      glw_model_token_free(t1);
      continue;
    }

    if(t->type == TOKEN_IDENTIFIER && t1 != NULL) {
      
      /**
       * Check if identifer is a function (i.e, it is followed by a
       * parenthesis)
       */
      if(t1->type == TOKEN_LEFT_PARENTHESIS) {
	/* Yep, try to resolve the identifier into a function */
	if(glw_model_function_resolve(t))
	  return glw_model_seterr(ei, t, "Unknown function: %s", 
				  rstr_get(t->t_rstring));

	t = t1->next;
	continue;
      }
    }
    t = t1;
  }
  return parse_shunting_yard(expr, ei);
}





/**
 *
 */
static int
parse_one_expression(token_t *prev, token_t *first, errorinfo_t *ei,
		     int *arraysep)
{
  token_t *t = first, *l = NULL;
  int balance = 0;

  while(t != NULL) {

    switch(t->type) {
    case TOKEN_END:
      glw_model_seterr(ei, first, "Unexpected end of file");
      return -1;

    case TOKEN_BLOCK_OPEN:
      if(parse_block(t, ei, TOKEN_BLOCK_CLOSE))
	return -1;
      break;

    case TOKEN_END_OF_EXPR:
      if(arraysep) {
	glw_model_seterr(ei, t, "Unexpected ';'");
	return -1;
      }
    end:
      t->type = TOKEN_EXPR;
      if(l == NULL) {
	t->type = TOKEN_NOP;
	/* Empty expression */
	return 0;
      }

      l->next = NULL;
      prev->next = t;
      t->child = first;
      return parse_prep_expression(t, ei);

    case TOKEN_BLOCK_CLOSE:
      glw_model_seterr(ei, t, "Unexpected '}'");
      return -1;

    case TOKEN_LEFT_BRACKET:
      if(parse_array(t, ei))
	return -1;
      break;
      
    case TOKEN_RIGHT_BRACKET:
      if(!arraysep) {
	glw_model_seterr(ei, t, "Unexpected ']'");
	return -1;
      }
      *arraysep = t->type;
      goto end;

    case TOKEN_LEFT_PARENTHESIS:
      balance++;
      break;

    case TOKEN_RIGHT_PARENTHESIS:
      balance--;
      break;

    case TOKEN_SEPARATOR:
      if(balance)
	break;
      if(arraysep == NULL) {
	glw_model_seterr(ei, t, "Unexpected ','");
	return -1;
      }
      *arraysep = t->type;
      goto end;

    default:
      break;
    }

    l = t;
    t = t->next;
  }
  return -1;
}

/**
 * A block is a set of {} (TOKEN_BLOCK_OPEN & TOKEN_BLOCK_CLOSE)
 * or it might be the entire file (TOKEN_START_OF_FILE & TOKEN_END_OF_FILE)
 *
 * A block consists of N expressions inside the block
 */
static int
parse_block(token_t *first, errorinfo_t *ei, token_type_t term)
{
  token_t *p;

  first->type = TOKEN_BLOCK;
  if(first->next->type == term) {
    p = first->next;
    first->next = p->next;
    glw_model_token_free(p);
    return 0;
  }

  p = first;

  while(p != NULL && p->next != NULL) {

    if(p->next->type == term) {

      first->child = first->next;
      first->next = p->next->next;
      glw_model_token_free(p->next);
      p->next = NULL;
      return 0;
    }

    if(parse_one_expression(p, p->next, ei, NULL))
      return -1;

    p = p->next;
  }

  glw_model_seterr(ei, first, "Unbalanced block");
  return -1;
}



/**
 * Parse an array, [ ... ]
 * Each element is comma separated
 */
static int
parse_array(token_t *first, errorinfo_t *ei)
{
  token_t *p;
  int expend = -1;

  first->type = TOKEN_ARRAY;
  if(first->next->type == TOKEN_RIGHT_BRACKET) {
    p = first->next;
    first->next = p->next;
    glw_model_token_free(p);
    return 0;
  }

  p = first;

  while(p != NULL && p->next != NULL) {

    if(expend == TOKEN_RIGHT_BRACKET) {

      first->child = first->next;
      first->next = p->next;
      p->next = NULL;
      return 0;
    }

    if(parse_one_expression(p, p->next, ei, &expend))
       return -1;
    p = p->next;
  }

  glw_model_seterr(ei, first, "Unbalanced block");
  return -1;
}


/**
 *
 */
int
glw_model_parse(token_t *sof, errorinfo_t *ei)
{
  return parse_block(sof, ei, TOKEN_END);
}
