"""Architecture documentation presence checks."""

import check_architecture_docs


def test_architecture_docs_required_snippets():
    assert check_architecture_docs.main() == 0
