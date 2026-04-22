all:

ifndef OVERRIDE_MAIN
clean:
	@rm -f *.log *.result
endif

# Status line formatting - shows running count of pass/fail
# Uses ANSI escape codes to update in place
define show_status
	@pass=$$(find . -maxdepth 1 -name '*.result' -exec grep -l '^PASS$$' {} + 2>/dev/null | wc -l); \
	fail=$$(find . -maxdepth 1 -name '*.result' -exec grep -l '^FAIL$$' {} + 2>/dev/null | wc -l); \
	if [ $$fail -gt 0 ]; then \
		printf '\r\033[K  [passed: %d, \033[31mFAILED: %d\033[0m]' $$pass $$fail; \
	else \
		printf '\r\033[K  [passed: %d, failed: 0]' $$pass; \
	fi
endef

# Run a single test, record result, show running status
# Arguments: $1 = test name, $2 = command to run
# Does NOT exit early on failure - runs all tests
define run_test
	@rc=0; \
	( set -e; $(2) ) >$1.log 2>&1 || rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		echo "PASS $1"; \
		echo PASS > $1.result; \
	else \
		echo "FAIL $1 (exit code $$rc)"; \
		echo FAIL > $1.result; \
	fi
	$(show_status)
endef

# Check results and exit non-zero if any tests failed
# Call this at the end of a test directory's execution
.PHONY: check-results
check-results:
	@echo ""
	@pass=$$(find . -maxdepth 1 -name '*.result' -exec grep -l '^PASS$$' {} + 2>/dev/null | wc -l); \
	fail=$$(find . -maxdepth 1 -name '*.result' -exec grep -l '^FAIL$$' {} + 2>/dev/null | wc -l); \
	if [ $$fail -gt 0 ]; then \
		echo ""; \
		echo "FAILED $$fail test(s) in $$(basename $$(pwd)):"; \
		find . -maxdepth 1 -name '*.result' -exec grep -l '^FAIL$$' {} + 2>/dev/null \
		  | sed 's|^\./||; s|\.result$$||' | while read t; do echo "  - $$t"; done; \
		exit 1; \
	fi

.PHONY: summary
summary:
	@pass=$$(find . -type f -name '*.result' -exec grep '^PASS$$' {} + 2>/dev/null | wc -l); \
	fail=$$(find . -type f -name '*.result' -exec grep '^FAIL$$' {} + 2>/dev/null | wc -l); \
	total=$$((pass + fail)); \
	echo "=========================="; \
	echo "Tests: $$total"; \
	echo "Passed: $$pass"; \
	echo "Failed: $$fail"; \
	echo "=========================="; \
	if [ $$fail -ne 0 ]; then \
		echo; \
		$(MAKE) --no-print-directory report; \
	fi; \
	test $$fail -eq 0

.PHONY: report
report:
	@echo "=========================="
	@echo "Failing tests:"
	@find . -name '*.result' -type f -exec grep -H '^FAIL$$' {} + \
	  | cut -d: -f1 \
	  | sed 's|^\./||; s|\.result$$||'
	@echo "=========================="
