# Testing and scoring

## botter check (after every change)

Validates the folder (config.json, AGENT.md, Lua syntax and doc comments, skills, scripts) and runs every
`test_*` in functions/tests and skills/*/functions/tests. It prints what each function reaches
("pure", "reads files", ...): check it matches what you meant. Fix every error and warning.

## botter eval: the score on solved examples

```
botter eval data/sample_tickets.csv
botter eval data/sample_tickets.csv --case T-1042
botter eval data/samples.csv --answers priority,team --id ticket_id --tol 1%
```

- Each example row minus its answer columns goes to `solve_case(case)` as a table of strings; the returned
  table is compared with the answers, column by column.
- Answer columns: the columns not in the input file next to the examples (e.g. tickets.csv), or
  `--inputs FILE`, or `--answers a,b`. Long free-text answers (explanations) are shown, not scored.
- Numbers match within `--tol` (default 2%, at least one cent); words and dates exactly, in any case
  (`Replied` = `replied`); lists (`a|b` or `a;b`) item by item in any order, each item's `:` parts too.
- Cases are named by the first column when it is a short unique id, else by row number (`--case 3`).
- When the project defines `prepare(cases)`, eval calls it once with every example first (so model readings
  are batched and cached), then `solve_case` for each.
- Output: right per column, whole rows, the mean column score, then every wrong value
  (`id: column: got X, want Y`), then the model use. `--case <id>` shows one example in full with every
  `bot.say` note the agent printed: add `bot.say` lines for intermediate values while you debug.
- `--json` for scripts; `--min 0.8` exits 1 below that mean column score. A case that raises an error
  makes the run exit 1 and shows the agent's last notes.
- eval runs the real agent, so `bot.llm` makes real calls: cache readings (references/flow.md).

## Improving the score without fooling yourself

1. Get a first honest version working end to end, then eval. A low score you can trust beats a high one you
   cannot explain.
2. Read the failures by column. Which column is worst? Is it one rule (due dates off by two days: the
   business-days rule) or noise (amounts a few percent off)?
3. Trace one failing case with `--case`. Print the intermediate values (`bot.say`) and compare them with
   what the brief says should happen. Fix the rule, not the case.
4. When the brief allows several readings ("within 3 days": calendar days or business days?),
   implement each behind a setting, eval each, and keep the one that scores best on all the examples. Write
   down which you chose and why. This is only for rules the brief leaves open. A rule it states plainly
   (a 30-day window, a minimum balance, a ranking order) stays as written even when another value scores
   higher: the examples are a sample, the brief is the spec. When the examples seem to contradict a stated
   rule, look for the real cause first (a wrong forecast, a missed event); if none is found, keep the rule
   and report the conflict with the cases that show it.
5. After each change eval again; keep it only if the score went up. Changes that help one example and
   hurt others are overfitting.
6. Numbers that are almost right everywhere often mean the examples were made from values the data does not
   show exactly. Get the decisions right first; do not chase the last cent.
7. Report what you measured: per column, whole rows, how many model calls a full run makes.

## Examples you were not given

If the job has no solved examples, make a few from the brief's own worked examples, label them by hand,
and keep them in evals/ (never embedded). Test the edge cases the brief names.
