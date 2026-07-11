@echo off
buildt\ember_cli.exe run self_hosted\parse_test.ember --fn main
echo EXITCODE=%ERRORLEVEL% > tmp_exit.txt
