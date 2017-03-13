# webbench-plus-post
This is a project based on webbench-1.5. The POST method was added, and users can specify multiple custom HTTP headers.

Installation:
make && make install.

Usage:
Content-Type: application/x-www-form-urlencoded
webbench --post content --header header1:value1 --header header2:header2 -t time -c number http://host/url

Content-Type: multipart/form-data; boundary=random_bytes_or_numbers
webbench --post filename --file --header header1:value1 --header header2:header2 -t time -c number http://host/url

