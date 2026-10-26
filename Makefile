.PHONY: package
package:
	pip install -e .

.PHONY: test
test: package
	python3 -m unittest -v ${tests}

.PHONY: quicktest
quicktest:
	python3 -m unittest -v ${tests}

.PHONY: format
format:
	ruff format .

.PHONY: check
check:
	ruff format --check .
	ruff check .
