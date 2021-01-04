avrdude -c usbasp -p t85 -U lfuse:w:0xe2:m -U hfuse:w:0xd7:m -U efuse:w:0xff:m -U flash:w:tinyDFPlayer_v1.1.hex
