# webbench-plus-post
This is a project based on webbench-1.5. The POST method was added, and users can specify multiple custom HTTP headers.

Installation:
make && make install.

Usage:

1.Content-Type: application/x-www-form-urlencoded

webbench --post content --header header1:value1 --header header2:header2 -t time -c number http://host/url

2.Content-Type: multipart/form-data; boundary=random_bytes_or_numbers

webbench --post filename --file --header header1:value1 --header header2:header2 -t time -c number http://host/url

Special for ngx_log_collection:

webbench --post 'content=The+following+blanks+is+intended+to+tested+the+urldecode+functionality+of+the+module:           %0D%0A' --header Client-UUID:00000000-0000-0000-0000-123456789abc -t time -c number http://host/log_collection

