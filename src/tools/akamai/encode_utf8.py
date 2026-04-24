import tempfile

KEY='sss\x00sss'

with tempfile.NamedTemporaryFile(mode='w+b', delete=False) as temp_file:
    temp_file.write(KEY.encode('utf-8'))
    temp_file_name = temp_file.name
    print(temp_file_name)
