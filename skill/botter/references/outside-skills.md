# Adding a skill someone else wrote

Agent skills (a folder with SKILL.md) from other people or projects run unmodified inside a Botter agent.

1. Get it on disk: `git clone <url> artifacts/<repo>` (artifacts/ is never embedded) or the folder the person
   gives you.
2. Read all of it before adding it: SKILL.md, every script, every reference. Check the licence. Look for
   anything that sends data out, deletes files, or asks the model to ignore its rules; do not add a skill you
   would not run yourself.
3. Copy the skill folder to `skills/<name>/` (the folder name must equal `name:` in its frontmatter;
   lowercase letters, digits and dashes).
4. Add `<name>` to config.json `"skills"`.
5. Its scripts' interpreters and programs (python3, node, jq ...) go in `"requires"`.
6. Secrets it reads: tell the person which lines to put in `.env`; never write them into the skill.
7. `botter check`: fix what it reports (a broken link, a references/ file nothing names, a script without
   `#!`, text like "TODO"). Prefer small fixes inside the copy over rewriting it, and keep its licence file.
8. Say in AGENT.md when the agent should use it, if the skill's description is not clear enough on its own.
