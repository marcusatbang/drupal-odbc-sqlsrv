<?php

class SelectQuery_odbc extends SelectQuery {
  private $debug = false;

  private function log($message, $variables, $severity) {
    if (is_null($variables))
      $variables = array();
    $variables['this'] = $this;
    if ($this->debug)
      $variables['threshold'] = WATCHDOG_DEBUG;
    $this->connection->log('SelectQuery_odbc: '.$message, $variables, $severity);
  }

  public function debug() {
    $this->debug = true;
    $this->log('Enabling debug');
    return $this;
  }

  /**
   * Override for SelectQuery::preExecute().
   *
   * Ensure that all the fields in ORDER BY and GROUP BY are part of the
   * main query.
   */
  public function preExecute(SelectQueryInterface $query = null) {
    // If no query object is passed in, use $this.
    if (!isset($query)) {
     $query = $this;
    }

    // Only execute this once.
    if ($this->isPrepared()) {
     return true;
    }

    // Execute standard pre-execution first.
    parent::preExecute($query);

    if ($this->distinct || $this->group) {
      // When the query is DISTINCT or contains GROUP BY fields, all the fields
      // in the GROUP BY and ORDER BY clauses must appear in the returned
      // columns.
      $columns = $this->order + array_flip($this->group);
      $counter = 0;
      foreach ($columns as $field => $dummy) {
        if (!isset($this->fields[$field]) && !isset($this->expressions[$field])) {
          $alias = '_field_' . ($counter++);
          $this->addExpression($field, $alias);
          // TODO: Do we want to rename this option?
          $this->queryOptions['sqlsrv_drop_columns'][] = $alias;
        }
      }

      // More over, GROUP BY columns cannot use aliases, so expand them to
      // their full expressions.
      foreach ($this->group as $key => &$group_field) {
        // Expand an alias on a field.
        if (isset($this->fields[$group_field])) {
          $field = $this->fields[$group_field];
          $group_field = (isset($field['table']) ? $this->connection->escapeTable($field['table']) . '.' : '') . $this->connection->escapeField($field['field']);
        }
        // Expand an alias on an expression.
        else if (isset($this->expressions[$group_field])) {
          $expression = $this->expressions[$group_field];
          $group_field = $expression['expression'];
        }
      }
    }

    // if ($this->debug) { echo "<!-- Select query: "; print_r($this->__toString()); echo " -->\n"; }

    $this->connection->preprocessExpressions($this->expressions, $this->group);
    return $this->prepared;
  }

  /**
   * Override for SelectQuery::compile().
   *
   * Detect when this query is prepared for use in a sub-query.
   */
  public function compile(DatabaseConnection $connection, QueryPlaceholderInterface $queryPlaceholder) {
  	// Detect when this query is prepared for use in a sub-query
    $this->inSubQuery = $queryPlaceholder != $this;
    $ok = parent::compile($connection, $queryPlaceholder);

    // Cannot use a range without an order, so add an artificial one.
    if (!empty($this->range)) {
      if (empty($this->order)) {
        $this->order["@@VERSION"] = "ASC";
        $this->expressions[] = array('expression' => "@@VERSION", 'alias' => '_VERSION', 'arguments' => array());
        $this->distinct = false;
      }
    }

    return $ok;
  }

  /**
   * Override for SelectQuery::execute().
   *
   * Show debugging
   */
  public function execute() {
    if ($this->debug) $this->queryOptions['DEBUG'] = true;
    try {
      if (!$this->preExecute()) {
        return NULL;
      }

      $queryString = (string) $this;
      $this->log('execute: %query with args: %args', array('%query' => (string) $queryString, '%args' => $this->getArguments()), WATCHDOG_INFO);

      if (!empty($this->range)) {
        $this->queryOptions['range'] = $this->range;
      }
      $query = parent::execute();
      return $query;
    } catch (Exception $e) {
      if (ODBC_LOGGING_DEBUG) echo "Exception in SelectQuery_odbc.execute()\n";
      $this->log('execute: '.get_class($e).' when executing "%sql": '.$e->getMessage(), array('%sql' => $queryString, '%args' => $this->getArguments()), WATCHDOG_ERROR);
      throw $e;
    }
  }

  public function __toString() {
    // Make sure this is done before the SQL is generated
    $this->preExecute();

    // For convenience, we compile the query ourselves if the caller forgot
    // to do it. This allows constructs like "(string) $query" to work. When
    // the query will be executed, it will be recompiled using the proper
    // placeholder generator anyway.
    if (!$this->compiled()) {
      $this->compile($this->connection, $this);
    }

    // Create a sanitized comment string to prepend to the query.
    $comments = $this->connection->makeComment($this->comments);

    // SELECT
    $query = $comments . 'SELECT ';
    if ($this->distinct) {
      $query .= 'DISTINCT ';
    }

    // RANGE
    // There is no universal SQL standard for handling range or limit clauses.
    // This is the SQL Server version, which has no offset capability so it has
    // to select from the top to the desired range and fix up the results later.
    // The WITH TIES prevents it selecting the wrong range, at the cost of it
    // sometimes returning an even bigger range of results
    if (!empty($this->range)) {
      $top = (int) $this->range['length'] + (int) $this->range['start'];
      $query .= "TOP(" . $top . ") ";
      if ($top > 1) $query .= "WITH TIES ";
    }


    // FIELDS and EXPRESSIONS
    $fields = array();
    foreach ($this->tables as $alias => $table) {
      if (!empty($table['all_fields'])) {
        $fields[] = $this->connection->escapeTable($alias) . '.*';
      }
    }
    foreach ($this->fields as $alias => $field) {
      // Always use the AS keyword for field aliases, as some
      // databases require it (e.g., PostgreSQL).
      $fields[] = (isset($field['table']) ? $this->connection->escapeTable($field['table']) . '.' : '') . $this->connection->escapeField($field['field']) . ' AS ' . $this->connection->escapeField($field['alias']);
    }
    foreach ($this->expressions as $alias => $expression) {
      $fields[] = $expression['expression'] . ' AS ' . $expression['alias'];
    }
    $query .= implode(', ', $fields);

    // FROM - We presume all queries have a FROM, as any query that doesn't won't need the query builder anyway.
    $query .= "\nFROM ";
    foreach ($this->tables as $alias => $table) {
      $query .= "\n";
      if (isset($table['join type'])) {
        $query .= $table['join type'] . ' JOIN ';
      }

      // If the table is a subquery, compile it and integrate it into this query.
      if ($table['table'] instanceof SelectQueryInterface) {
        // Run preparation steps on this sub-query before converting to string.
        $subquery = $table['table'];
        $subquery->preExecute();
        $table_string = '(' . (string) $subquery . ')';
      }
      else {
        $table_string = '{' . $this->connection->escapeTable($table['table']) . '}';
      }

      // Don't use the AS keyword for table aliases, as some
      // databases don't support it (e.g., Oracle).
      $query .=  $table_string . ' ' . $this->connection->escapeTable($table['alias']);

      if (!empty($table['condition'])) {
        $query .= ' ON ' . $table['condition'];
      }
    }

    // WHERE
    if (count($this->where)) {
      // There is an implicit string cast on $this->condition.
      $query .= "\nWHERE ( " . $this->where . " )";
    }

    // GROUP BY
    if ($this->group) {
      $group = array_filter($this->group, array($this, 'validGroupBy'));
      $query .= "\nGROUP BY " . implode(', ', $group);
    }

    // HAVING
    if (count($this->having)) {
      // There is an implicit string cast on $this->having.
      $query .= "\nHAVING " . $this->having;
    }

    // ORDER BY
    // The ORDER BY clause is invalid in views, inline functions, derived
    // tables, subqueries, and common table expressions, unless TOP or FOR XML
    // is also specified.
    if ($this->order && (empty($this->inSubQuery) || !empty($this->range))) {
      $query .= "\nORDER BY ";
      $fields = array();
      foreach ($this->order as $field => $direction) {
        $fields[] = $field . ' ' . $direction;
      }
      $query .= implode(', ', $fields);
    }

    // UNION is a little odd, as the select queries to combine are passed into
    // this query, but syntactically they all end up on the same level.
    if ($this->union) {
      foreach ($this->union as $union) {
        $query .= ' ' . $union['type'] . ' ' . (string) $union['query'];
      }
    }

    $this->log('Query string: %query', array('%query' => $query), WATCHDOG_DEBUG);
    return $query;
  }

  /**
   * Work around an annoying feature of SQL Server where it refuses to group by literal values
   * http://www.sql-server-helper.com/error-messages/msg-164.aspx
   */
  protected function validGroupBy($group) {
    if (is_numeric($group)) return false;
    if (is_string($group)) {
      $group = trim($group);
      $ch0 = substr($group, 0, 1);
      if ($ch0 == "'") return false;
    }
    return true;
  }

  /**
   * Override of SelectQuery::orderRandom() for SQL Server.
   *
   * It seems that sorting by RAND() doesn't actually work, this is a less then
   * elegant workaround.
   *
   * @status tested
   */
  public function orderRandom() {
    $alias = $this->addExpression('NEWID()', 'random_field');
    $this->orderBy($alias);
    return $this;
  }
}
