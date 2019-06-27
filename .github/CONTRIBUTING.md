## Contributing

This project welcomes contributions and suggestions. Most contributions require you to
agree to a Contributor License Agreement (CLA) declaring that you have the right to,
and actually do, grant us the rights to use your contribution. For details, visit
https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need
to provide a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the
instructions provided by the bot. You will only need to do this once across all repositories using our CLA.

All pull requests must pass a suite of CI tests before they will be merged. The test
commands are defined in `.vsts-ci.yml` and `.circleci/config.yml`, so you can locally
repeat any tests which fail. You should at least run the `static_checks` job before
creating a pull request, ensuring all of your code is correctly formatted. The test
commands will only report misformatted files - to _reformat_ the files, pass `-f`
to the `check-format.sh ...` command and remove `--check` from the `black ...` command.
