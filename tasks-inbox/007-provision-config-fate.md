# Decide what `provision_config.py` is for after onboarding

Onboarding makes the script nearly redundant for a human: the board reads its own
id and writes its own document, which is the whole reason the script exists.

CI still copies a template into `data/config.json` before `-t buildfs`, so
something has to keep doing that.

**Done means:** either the script is scoped down to the CI job and says so in its
own docstring, or it is deleted and CI's `cp` is enough. Not left ambiguous —
two ways to provision a board is how the wrong one gets used.

Blocked on 002 and 006.
