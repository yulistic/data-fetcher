test_inc = include_directories('.')
test_inc_dirs = [inc, log_inc, test_inc]

## Tests
df_server_test_exe = executable('df_server_test', 'df_server_test.c',
	include_directories : test_inc_dirs, link_with : data_fetcher_lib)

df_client_test_exe = executable('df_client_test', 'df_client_test.c',
	include_directories : test_inc_dirs, link_with : data_fetcher_lib)

# Do test.
# test('df_server_test', df_server_test_exe, suite : 'df', is_parallel : true, priority : -50) # default priority = 0
# test('df_client_test', df_client_test_exe, suite : 'df', is_parallel : true)