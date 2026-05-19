CREATE TABLE IF NOT EXISTS measurements
(
    id SERIAL PRIMARY KEY,

    -- =========================
    -- LOCATION
    -- =========================

    time TEXT,

    latitude DOUBLE PRECISION,
    longitude DOUBLE PRECISION,
    altitude DOUBLE PRECISION,
    accuracy DOUBLE PRECISION,

    -- =========================
    -- COMMON
    -- =========================

    network_type TEXT,

    -- =========================
    -- LTE
    -- =========================

    lte_band TEXT,
    lte_ci BIGINT,
    lte_earfcn INTEGER,
    lte_mcc TEXT,
    lte_mnc TEXT,
    lte_pci INTEGER,
    lte_tac INTEGER,

    lte_asu_level INTEGER,
    lte_cqi INTEGER,

    lte_rsrp INTEGER,
    lte_rsrq INTEGER,
    lte_rssi INTEGER,
    lte_rssnr INTEGER,

    lte_timing_advance INTEGER,

    -- =========================
    -- GSM
    -- =========================

    gsm_cid BIGINT,
    gsm_bsic INTEGER,
    gsm_arfcn INTEGER,
    gsm_lac INTEGER,
    gsm_mcc TEXT,
    gsm_mnc TEXT,
    gsm_psc INTEGER,

    gsm_dbm INTEGER,
    gsm_rssi INTEGER,
    gsm_timing_advance INTEGER,

    -- =========================
    -- NR (5G)
    -- =========================

    nr_band TEXT,
    nr_nci BIGINT,
    nr_pci INTEGER,
    nr_nrarfcn INTEGER,
    nr_tac INTEGER,
    nr_mcc TEXT,
    nr_mnc TEXT,

    nr_ss_rsrp INTEGER,
    nr_ss_rsrq INTEGER,
    nr_ss_sinr INTEGER,

    nr_timing_advance INTEGER
);