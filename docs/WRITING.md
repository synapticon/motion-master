# Writing Guide

This guide sets the English style for everything written down in this repository. It
applies to documentation, to code comments, to commit messages, and to issues.

The base is **ASD-STE100 Simplified Technical English**. STE is a controlled language.
The aerospace industry built it so that a reader who cannot ask a follow-up question
reads the text one way only. Its rules are countable, so you can check prose against
them in review.

Many readers of this repository read English as a second language. Machine builders,
integrators, and anyone who forks the project must be able to act on these documents
without a native speaker beside them. That is the reason for the rules below.

## Clarity is the goal, not concision

Read this before the rule table, because it decides every conflict the table creates.

The sentence limits apply to each sentence. They do not apply to the whole text. A long
document in short sentences is correct.

Never drop a fact, a condition, a caveat, or a limit to meet a rule. Split the sentence
instead. A short document that omits a caveat is worse than a long document that keeps
it.

Do not pad either. If one sentence carries the meaning, do not write three.

## Base rules

| Rule | Limit |
| --- | --- |
| Noun clusters | Maximum 3 words stacked as a modifier. Break a longer stack apart and name the relationship. |
| Sentence length | Maximum 20 words for an instruction or a procedure. Maximum 25 words for descriptive text. |
| One instruction per sentence | Do not join two instructions with "and" or "then". |
| Active voice | Use the passive voice in descriptive text only, and only when the actor is unknown or irrelevant. |
| Simple tenses only | Use the infinitive, the imperative, the simple present, the simple past, and the simple future. Use a past participle as an adjective only. Do not use the present perfect or the past perfect. |
| No `-ing` verb forms | Use an `-ing` word as a technical noun, or as part of one, only. |
| No hedge stacking | Do not chain modal verbs, as in "may have been caused by". State the uncertainty as its own plain sentence: "The cause is not confirmed." |
| One word, one meaning | Use one term for one concept and repeat it. Do not rotate synonyms for the same idea. |
| Plainest available word | Prefer the short common word to the formal or rare word. |
| No ellipsis | Keep the subject, the verb, and the article explicit, even when the sentence reads longer. |
| Paragraphs | One topic. Maximum 6 sentences. |
| Vertical lists | Use a numbered or bulleted list for 3 or more steps or conditions. |

## Documentation

This covers `README.md`, `CHANGELOG.md`, `ANNOUNCEMENT.md`, and every file in `docs/`.

Apply every base rule. Then apply these:

- **Define a domain term at first use.** Write "SDO (Service Data Object, a mailbox read
  or write of one dictionary entry)" the first time, then "SDO" after that.
- **Assume no prior knowledge of this repository.** The reader may have found it an hour
  ago. Do not assume the reader knows `NEXTGEN.md`.
- **Show the command.** A command block beats a description of the command.
- **Target middle verbosity.** Give enough to follow the steps and to understand the
  shape. Do not write the design rationale here. That belongs in `NEXTGEN.md`.
- **State the audience of a procedure** when it is not obvious. Some steps run on the
  server. Some steps run on the client machine.

## Code comments

The generated Doxygen site comes from these comments. Apply every base rule. Then apply
these:

- **Do not define common domain terms.** The reader of a source file already knows what
  SDO, PDO, FMMU, and AL state mean. A definition here is noise. This is the one place
  where the documentation rule above does not apply.
- **Say why, not what.** The code says what it does. Explain the reason it does it that
  way.
- **Record what you learn.** A comment that captures a measurement, a hardware
  behaviour, or a trap is the most valuable comment in the file. Keep those. Write them
  in full.
- **State the present.** Never describe what the code was before. Never describe what a
  change replaced. History belongs in the commit message.
- **Never cite a count that will change.** Do not write "~23 OS commands". Describe the
  mechanism instead.
- **Never explain behaviour you only inferred.** Read the firmware source, measure it, or
  ask. Mark a guess as a guess.
- **Keep a `@brief` to one line.** Start it with a verb in the simple present, or with
  the noun the function returns.

## Commit messages and pull requests

Apply every base rule. Then apply these:

- **Use a conventional-commit subject**: `type(scope): summary`. Types in use are `feat`,
  `fix`, `refactor`, `style`, `chore`, `docs`, `test`.
- **Write the subject in the imperative and the simple present.** "add", not "added" and
  not "adds".
- **The body says why.** The diff says what.
- **Never reference an older version of this software** by name or by number in a commit
  message.

## Issues

One issue tracks one feature or one defect. Apply every base rule. Then apply these:

- **Write the title as the plain feature name.** Do not add a type prefix or a scope.
- **Open with a user story.** Write three lines: "As a ...", then "I want ...", then "so
  that ...". End the first two lines with two spaces, so GitHub keeps the break.
- **Use these sections, in this order.** User story, Context, Acceptance criteria,
  Implementation, Out of scope, Notes, References. Leave out a section you have nothing
  to say in.
- **Write each acceptance criterion as a checkbox.** One criterion states one outcome a
  reviewer can test.
- **Say where the behaviour exists today** in Context, and why this repository does it
  differently.
- **Never wrap a line.** One paragraph is one line. One list item is one line. GitHub
  wraps the text for the reader, and a wrapped source is harder to edit. The user story
  is the one exception.
- **Point at the file, not at the idea.** Give the path a reader must open.

## What these rules never touch

Do not apply any rule above to:

- **Code.** This includes identifiers, syntax, and string literals.
- **Quoted material.** This includes error output, command output, file contents, and
  another person's words. To rewrite a quotation is falsification, not simplification.
- **Text where the exact wording carries the meaning.** This includes a command to run,
  an API path, a config key, and an exact error string.
- **Replies in an interactive session.** A different style governs those.

## Precedence

A more specific instruction wins over this guide. This includes an instruction from a
maintainer, an instruction in `CLAUDE.md`, and an established convention in the file you
edit.

Follow the more specific instruction without comment. Do not cite this guide as a reason
to override it.

This exception covers an explicit instruction only. Do not relax a rule because a topic
feels casual, or because nearby prose reads more freely.

## Project vocabulary

STE lets a project define its own approved terms. This repository does not keep a
glossary file yet. Until it does, follow two rules:

1. Use the term the code uses. If the type is `FieldbusDriver`, write "fieldbus driver",
   never "bus adapter".
2. Use one term for one concept across the whole repository. Do not write "slave" in one
   file and "device" in another for the same thing.
