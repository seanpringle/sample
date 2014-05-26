# SAMPLE

MariaDB storage engine for sampling stuff.

* INSERT always succeeds, but only sampled rows are stored.
* SELECT returns all sampled rows and truncates the table.
* In-memory tables only; data will not survive restart.
* Configurable sample rate and memory limit.
* Concurrent inserts.

### Example: General Query Log

    SET GLOBAL sample_rate=1000;
    SET GLOBAL sample_limit=1000000;
    ALTER TABLE mysql.general_log ENGINE=SAMPLE;