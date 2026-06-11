"""ESP_IDF_CONTRACT.md structure and scope checks."""

from check_esp_idf_contract import run_contract_check, run_scope_check


def test_contract_document_structure():
    assert run_contract_check() == 0


def test_out_of_scope_terms_stay_in_scope_section():
    assert run_scope_check() == 0
