-- qex benchmark query suite
-- Covers: range/equality filters, AND/OR/NOT, all four LIKE paths,
-- null handling, arithmetic projection, CSE, selectivity reorder,
-- same-structure/different-constant reuse, string equality via dict bitmap.

-- 1. simple numeric range
SELECT service, latency_us FROM logs
WHERE latency_us > 5000;

-- 2. equality on dictionary string
SELECT service, status FROM logs
WHERE service = 'payment';

-- 3. three-way AND (selectivity reorder candidate)
SELECT service, latency_us, status FROM logs
WHERE status >= 500 AND latency_us > 2000 AND service = 'payment';

-- 4. OR
SELECT service, status FROM logs
WHERE status = 404 OR status = 503;

-- 5. NOT
SELECT service FROM logs
WHERE NOT (status = 200);

-- 6. LIKE prefix path
SELECT service, msg FROM logs
WHERE msg LIKE 'payment%';

-- 7. LIKE contains path
SELECT service, msg FROM logs
WHERE msg LIKE '%timeout%';

-- 8. LIKE exact path (no wildcard)
SELECT service, msg FROM logs
WHERE msg LIKE 'cache hit served from memory';

-- 9. LIKE glob path (underscore + multiple %)
SELECT host, service FROM logs
WHERE host LIKE 'host-1_%';

-- 10. LIKE glob with two percent wildcards
SELECT msg FROM logs
WHERE msg LIKE '%failed%retry%';

-- 11. IS NULL
SELECT latency_us FROM logs
WHERE latency_us IS NULL;

-- 12. IS NOT NULL
SELECT latency_us FROM logs
WHERE latency_us IS NOT NULL;

-- 13. arithmetic projection
SELECT service, latency_us, latency_us / 1000, latency_us * 2 + 10
FROM logs WHERE latency_us > 1000;

-- 14. CSE: same subexpression repeated
SELECT latency_us,
       latency_us / 1000,
       latency_us / 1000 + 1,
       latency_us / 1000 * 2
FROM logs WHERE latency_us > 1000;

-- 15. CSE across filter and projection
SELECT service, latency_us / 1000
FROM logs WHERE latency_us / 1000 > 5;

-- 16. same structure, different constants (a)
SELECT service, latency_us FROM logs
WHERE latency_us > 3000;

-- 17. same structure, different constants (b) -> structural cache hit
SELECT service, latency_us FROM logs
WHERE latency_us > 9000;

-- 18. same structure, different constants (c) -> structural cache hit
SELECT service, latency_us FROM logs
WHERE latency_us > 12000;

-- 19. same string-compare structure, different constants -> cache hit
SELECT service FROM logs WHERE service = 'auth';

-- 20. same string-compare structure, another constant -> cache hit
SELECT service FROM logs WHERE service = 'cart';

-- 21. nested arithmetic in filter
SELECT service, latency_us, resp_bytes
FROM logs WHERE latency_us + resp_bytes > 10000;

-- 22. parentheses and mixed AND/OR
SELECT service, status, latency_us FROM logs
WHERE (status >= 500 OR latency_us > 8000) AND service = 'order';

-- 23. chained AND, deliberately worst order first (reorder showcase)
SELECT service FROM logs
WHERE cpu_pct > 90 AND status = 500 AND service = 'auth';

-- 24. modulo and comparison
SELECT service, status FROM logs
WHERE status % 100 = 0;

-- 25. float comparison
SELECT service, cpu_pct FROM logs
WHERE cpu_pct > 95.5;

-- 26. combined null-aware predicate
SELECT service, latency_us, resp_bytes FROM logs
WHERE latency_us IS NOT NULL AND resp_bytes IS NOT NULL
  AND latency_us > 4000;

-- 27. string inequality (dictionary ordered)
SELECT service FROM logs
WHERE service >= 'payment';

-- 28. LIKE prefix on another column
SELECT host FROM logs
WHERE host LIKE 'host-9%';

-- 29. heavy filter then arithmetic CSE
SELECT service, latency_us,
       (latency_us + resp_bytes) / 2,
       (latency_us + resp_bytes) % 1000
FROM logs
WHERE status >= 400 AND latency_us > 500;

-- 30. deep nested boolean expression
SELECT service, status FROM logs
WHERE NOT (service = 'auth' OR service = 'session')
  AND (status = 200 OR status = 201 OR status = 204)
  AND latency_us > 100;
