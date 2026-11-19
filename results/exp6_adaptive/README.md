# Experiment 6: finding hot pages automatically

- `results_exp6.csv`: every run. Rows: the correctness check (10 s, `check_errors` = 0), part A (plain / known hot set = `filerep` / adaptive under 12 and 10 GiB limits, 2 × 30 s each), the 20 s learning-curve run, and part B (60 s runs with `shift_s` = 20).
- `timeline_learn.csv`: adaptive replicas from a cold start, throughput every second.
- `timeline_move_<setting>_<run>.csv`: part B, throughput every second; the hot set moves at 20 s (`hot_base_block` changes).

Chart: [figures/exp6_moving.png](../../figures/exp6_moving.png). Re-draw from these files with `python3 ../../code/plot_timeline.py` run inside this folder.
- `results_exp6b_tuning.csv`: replica area 10/12/15% and threshold 4/6/8 (last column `threshold`).
- `results_exp6c_low_threshold.csv`: threshold 2 and 3 with a 10% area.
