cmd_/home/mlpower/MLPower/src/test_module/test.mod := printf '%s\n'   test.o | awk '!x[$$0]++ { print("/home/mlpower/MLPower/src/test_module/"$$0) }' > /home/mlpower/MLPower/src/test_module/test.mod
