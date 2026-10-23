# Before botter build

- [ ] Every rule in the brief maps to code (or to AGENT.md for chat behaviour); you can point to the line.
- [ ] No hard-coded answers, no reading the examples' answer columns, no mock or fixed results.
- [ ] Every rule the brief states plainly is implemented as written, not tuned to score better on the
      examples; any conflict between the brief and the examples is in the report.
- [ ] The logic lives in the Lua project only (no second copy in another language left in the folder).
- [ ] Model readings are typed (`schema`), validated in code, cached, and spot-checked on real cases.
- [ ] Data texts and images are treated as untrusted data in every prompt.
- [ ] `botter check`: 0 errors, 0 warnings, tests for each rule with real rows.
- [ ] `botter eval` on the solved examples (if any): you know the score per column and why the misses miss.
- [ ] config.json: name, version, one-sentence description, `"temperature": 0` for batch, every program in
      `"requires"` (empty if pure Markdown + Lua), skills listed.
- [ ] No secrets in any project file.
- [ ] `botter build`, then `./<name> --about`, then one real run the way its users will run it
      (`./<name> --run "..."`, or a short chat), and read the output.
- [ ] The report states what was measured: score per column, whole rows, model calls and tokens for a full
      run, and every choice you made where the brief was unclear.
