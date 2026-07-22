PYTHON ?= python3

.PHONY: all test clean

all:
	$(PYTHON) -m py_compile neptune_edge_host.py neptune_ctl.py neptune_rx.py

test:
	$(MAKE) -C ../../ps/control-daemon all
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v
	$(PYTHON) neptune_ctl.py --help >/dev/null
	$(PYTHON) neptune_rx.py --help >/dev/null

clean:
	rm -rf __pycache__ tests/__pycache__
