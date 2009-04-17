SET( DOCS_COMMAND
  COMMAND xsltproc --nonet --xinclude http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl man-clicfs.1.docbook
  COMMAND xsltproc --nonet --xinclude http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl man-unclicfs.1.docbook
  COMMAND xsltproc --nonet --xinclude http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl man-mkclicfs.1.docbook
)

add_custom_target( docs ${DOCS_COMMAND} VERBATIM )

install(FILES clicfs.1 mkclicfs.1 unclicfs.1 DESTINATION share/man/man1)
