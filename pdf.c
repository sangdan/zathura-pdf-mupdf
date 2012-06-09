/* See LICENSE file for license and copyright information */

#include <stdlib.h>
#include <ctype.h>
#include <girara/datastructures.h>
#include <glib.h>

#include "pdf.h"

/* forward declarations */
static inline int text_span_char_at(fz_text_span *span, int index);
static unsigned int text_span_length(fz_text_span *span);
static int text_span_match_string_n(fz_text_span* span, const char* string,
    int n, zathura_rectangle_t* rectangle);
static void pdf_zathura_image_free(zathura_image_t* image);
static void get_images(fz_obj* dict, girara_list_t* list);
static void get_resources(fz_obj* resource, girara_list_t* list);
static void search_result_add_char(zathura_rectangle_t* rectangle,
    fz_text_span* span, int n);
static void mupdf_page_extract_text(pdf_xref* document,
    mupdf_page_t* mupdf_page);
static void build_index(mupdf_document_t* mupdf_document, pdf_outline* outline,
    girara_tree_node_t* root);

void
register_functions(zathura_plugin_functions_t* functions)
{
  functions->document_open            = (zathura_plugin_document_open_t) pdf_document_open;
  functions->document_free            = (zathura_plugin_document_free_t) pdf_document_free;
  functions->document_index_generate  = (zathura_plugin_document_index_generate_t) pdf_document_index_generate;
  functions->page_init                = (zathura_plugin_page_init_t) pdf_page_init;
  functions->page_clear               = (zathura_plugin_page_clear_t) pdf_page_clear;
  functions->page_search_text         = (zathura_plugin_page_search_text_t) pdf_page_search_text;
  functions->page_links_get           = (zathura_plugin_page_links_get_t )pdf_page_links_get;
#if 0
  functions->page_images_get          = (zathura_plugin_page_images_get_t) pdf_page_images_get;
#endif
  functions->page_get_text            = (zathura_plugin_page_get_text_t) pdf_page_get_text;
  functions->document_get_information = (zathura_plugin_document_get_information_t) pdf_document_get_information;
  functions->page_render              = (zathura_plugin_page_render_t) pdf_page_render;
#if HAVE_CAIRO
  functions->page_render_cairo        = (zathura_plugin_page_render_cairo_t) pdf_page_render_cairo;
#endif
}

ZATHURA_PLUGIN_REGISTER(
  "pdf-mupdf",
  VERSION_MAJOR, VERSION_MINOR, VERSION_REV,
  register_functions,
  ZATHURA_PLUGIN_MIMETYPES({
    "application/pdf"
  })
)

zathura_error_t
pdf_document_open(zathura_document_t* document)
{
  zathura_error_t error = ZATHURA_ERROR_OK;
  if (document == NULL) {
    error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = calloc(1, sizeof(mupdf_document_t));
  if (mupdf_document == NULL) {
    error = ZATHURA_ERROR_OUT_OF_MEMORY;
    goto error_ret;
  }

  fz_accelerate();
  mupdf_document->glyph_cache = fz_new_glyph_cache();

  if (pdf_open_xref(&(mupdf_document->document), zathura_document_get_path(document), NULL) != fz_okay) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  if (pdf_needs_password(mupdf_document->document) != 0) {
    const char* password = zathura_document_get_password(document);
    if (password == NULL || pdf_authenticate_password(mupdf_document->document, (char*) password) != 0) {
      error = ZATHURA_ERROR_INVALID_PASSWORD;
      goto error_free;
    }
  }

  if (pdf_load_page_tree(mupdf_document->document) != fz_okay) {
    error = ZATHURA_ERROR_UNKNOWN;
    goto error_free;
  }

  zathura_document_set_number_of_pages(document, pdf_count_pages(mupdf_document->document));
  zathura_document_set_data(document, mupdf_document);

  return error;

error_free:

  if (mupdf_document != NULL) {
    if (mupdf_document->document != NULL) {
      pdf_free_xref(mupdf_document->document);
    }

    if (mupdf_document->glyph_cache != NULL) {
      fz_free_glyph_cache(mupdf_document->glyph_cache);
    }

    free(mupdf_document);
  }

  zathura_document_set_data(document, NULL);

error_ret:

  return error;
}

zathura_error_t
pdf_document_free(zathura_document_t* document, mupdf_document_t* mupdf_document)
{
  if (document == NULL || mupdf_document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  pdf_free_xref(mupdf_document->document);
  fz_free_glyph_cache(mupdf_document->glyph_cache);
  free(mupdf_document);
  zathura_document_set_data(document, NULL);

  return ZATHURA_ERROR_OK;
}

girara_tree_node_t*
pdf_document_index_generate(zathura_document_t* document, mupdf_document_t* mupdf_document, zathura_error_t* error)
{
  if (document == NULL || mupdf_document == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  /* get outline */
  pdf_outline* outline = pdf_load_outline(mupdf_document->document);
  if (outline == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  /* generate index */
  girara_tree_node_t* root = girara_node_new(zathura_index_element_new("ROOT"));
  build_index(mupdf_document, outline, root);

  /* free outline */
  pdf_free_outline(outline);

  return root;
}

zathura_error_t
pdf_page_init(zathura_page_t* page)
{
  if (page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  zathura_document_t* document     = zathura_page_get_document(page);
  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  mupdf_page_t* mupdf_page         = calloc(1, sizeof(mupdf_page_t));

  if (mupdf_page == NULL) {
    return  ZATHURA_ERROR_OUT_OF_MEMORY;
  }

  zathura_page_set_data(page, mupdf_page);

  /* load page */
  if (pdf_load_page(&(mupdf_page->page), mupdf_document->document, zathura_page_get_index(page)) != fz_okay) {
    goto error_free;
  }

  /* get page dimensions */
  zathura_page_set_width(page,  mupdf_page->page->mediabox.x1 - mupdf_page->page->mediabox.x0);
  zathura_page_set_height(page, mupdf_page->page->mediabox.y1 - mupdf_page->page->mediabox.y0);

  /* setup text */
  mupdf_page->extracted_text = false;
  mupdf_page->text           = fz_new_text_span();
  if (mupdf_page->text == NULL) {
    goto error_free;
  }

  return ZATHURA_ERROR_OK;

error_free:

  if (mupdf_page != NULL) {
    if (mupdf_page->page != NULL) {
      pdf_free_page(mupdf_page->page);
    }

    if (mupdf_page->text != NULL) {
      fz_free_text_span(mupdf_page->text);
    }

    free(mupdf_page);
  }

  return ZATHURA_ERROR_UNKNOWN;
}

zathura_error_t
pdf_page_clear(zathura_page_t* page, mupdf_page_t* mupdf_page)
{
  if (page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  if (mupdf_page != NULL) {
    if (mupdf_page->text != NULL) {
      fz_free_text_span(mupdf_page->text);
    }

    if (mupdf_page->page != NULL) {
      pdf_free_page(mupdf_page->page);
    }

    free(mupdf_page);
  }

  return ZATHURA_ERROR_OK;
}

girara_list_t*
pdf_page_search_text(zathura_page_t* page, mupdf_page_t* mupdf_page, const char* text, zathura_error_t* error)
{
  if (page == NULL || text == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    goto error_ret;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  if (mupdf_page == NULL || mupdf_page->text == NULL) {
    goto error_ret;
  }

  /* extract text (only once) */
  if (mupdf_page->extracted_text == false) {
    mupdf_page_extract_text(mupdf_document->document, mupdf_page);
  }

  girara_list_t* list = girara_list_new2((girara_free_function_t) zathura_link_free);
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    goto error_free;
  }

  double page_height = zathura_page_get_height(page);

  unsigned int length = text_span_length(mupdf_page->text);
  for (int i = 0; i < length; i++) {
    zathura_rectangle_t* rectangle = g_malloc0(sizeof(zathura_rectangle_t));

    /* search for string */
    int match = text_span_match_string_n(mupdf_page->text, text, i, rectangle);
    if (match == 0) {
      g_free(rectangle);
      continue;
    }

    rectangle->y1 = page_height - rectangle->y1;
    rectangle->y2 = page_height - rectangle->y2;

    girara_list_append(list, rectangle);
  }

  return list;

error_free:

  if (list != NULL ) {
    girara_list_free(list);
  }

error_ret:

  if (error != NULL && *error == ZATHURA_ERROR_OK) {
    *error = ZATHURA_ERROR_UNKNOWN;
  }

  return NULL;
}

girara_list_t*
pdf_page_links_get(zathura_page_t* page, mupdf_page_t* mupdf_page, zathura_error_t* error)
{
  if (page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    goto error_ret;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL || mupdf_page == NULL || mupdf_page->page == NULL) {
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);;

  girara_list_t* list = girara_list_new2((girara_free_function_t) zathura_link_free);
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    goto error_free;
  }

  double page_height = zathura_page_get_height(page);

  pdf_link* link = mupdf_page->page->links;
  for (; link != NULL; link = link->next) {
    /* extract position */
    zathura_rectangle_t position;
    position.x1 = link->rect.x0;
    position.x2 = link->rect.x1;
    position.y1 = page_height - link->rect.y1;
    position.y2 = page_height - link->rect.y0;

    zathura_link_type_t type     = ZATHURA_LINK_INVALID;
    zathura_link_target_t target = { 0 };

    char* buffer = NULL;
    switch (link->kind) {
      case PDF_LINK_URI:
        buffer = g_malloc0(sizeof(char) * (fz_to_str_len(link->dest) + 1));
        memcpy(buffer, fz_to_str_buf(link->dest), fz_to_str_len(link->dest));
        buffer[fz_to_str_len(link->dest)] = '\0';

        type         = ZATHURA_LINK_URI;
        target.value = buffer;
        break;
      case PDF_LINK_GOTO:
        type               = ZATHURA_LINK_GOTO_DEST;
        target.page_number = pdf_find_page_number(mupdf_document->document,
            fz_array_get(link->dest, 0));
        break;
      default:
        continue;
    }

    zathura_link_t* zathura_link = zathura_link_new(type, position, target);
    if (zathura_link != NULL) {
      girara_list_append(list, zathura_link);
    }

    if (buffer != NULL) {
      g_free(buffer);
    }
  }

  return list;

error_free:

  if (list != NULL) {
    girara_list_free(list);
  }

error_ret:

  return NULL;
}

char*
pdf_page_get_text(zathura_page_t* page, mupdf_page_t* mupdf_page, zathura_rectangle_t rectangle, zathura_error_t* error)
{
  if (page == NULL || mupdf_page == NULL || mupdf_page->text == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    goto error_ret;
  }

  GString* text      = g_string_new(NULL);
  double page_height = zathura_page_get_height(page);

  for (fz_text_span* span = mupdf_page->text; span != NULL; span = span->next) {
    bool seen = false;

    for (int i = 0; i < span->len; i++) {
      fz_bbox hitbox = fz_transform_bbox(fz_identity, span->text[i].bbox);
      int c = span->text[i].c;

      if (c < 32) {
        c = '?';
      }

      if (hitbox.x1 >= rectangle.x1
          && hitbox.x0 <= rectangle.x2
          && (page_height - hitbox.y1) >= rectangle.y1
          && (page_height - hitbox.y0) <= rectangle.y2) {
        g_string_append_c(text, c);
        seen = true;
      }
    }

    if (seen == true && span->eol != 0) {
      g_string_append_c(text, '\n');
    }
  }

  if (text->len == 0) {
    g_string_free(text, TRUE);
    return NULL;
  } else {
    char* t = text->str;
    g_string_free(text, FALSE);
    return t;
  }

error_ret:

  if (error != NULL && *error == ZATHURA_ERROR_OK) {
    *error = ZATHURA_ERROR_UNKNOWN;
  }

  return NULL;
}

girara_list_t*
pdf_page_images_get(zathura_page_t* page, mupdf_page_t* mupdf_page, zathura_error_t* error)
{
  if (page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    goto error_ret;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    goto error_ret;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  fz_obj* page_object = mupdf_document->document->page_objs[zathura_page_get_index(page)];
  if (page_object == NULL) {
    goto error_free;
  }

  fz_obj* resource = fz_dict_gets(page_object, "Resources");
  if (resource == NULL) {
    goto error_free;
  }

  girara_list_t* list = girara_list_new();
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    goto error_free;
  }

  girara_list_set_free_function(list, (girara_free_function_t) pdf_zathura_image_free);

  get_resources(resource, list);

  return list;

error_free:

  if (error != NULL && *error == ZATHURA_ERROR_OK) {
    *error = ZATHURA_ERROR_UNKNOWN;
  }

  if (list != NULL) {
    girara_list_free(list);
  }

error_ret:

  return NULL;
}

girara_list_t*
pdf_document_get_information(zathura_document_t* document, mupdf_document_t*
    mupdf_document, zathura_error_t* error)
{
  if (document == NULL || mupdf_document == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  fz_obj* object = fz_dict_gets(mupdf_document->document->trailer, "Info");
  fz_obj* info   = fz_resolve_indirect(object);

  girara_list_t* list = zathura_document_information_entry_list_new();
  if (list == NULL) {
    return NULL;
  }

  for (int i = 0; i < fz_dict_len(info); i++) {
    fz_obj* key = fz_dict_get_key(info, i);
    fz_obj* val = fz_dict_get_val(info, i);

    if (fz_is_name(key) == 0 || fz_is_string(val) == 0) {
      continue;
    }

    char* name  = fz_to_name(key);
    char* value = fz_to_str_buf(val);
    zathura_document_information_type_t type = ZATHURA_DOCUMENT_INFORMATION_OTHER;

    if (strcmp(name, "Author") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_AUTHOR;
    } else if (strcmp(name, "Title") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_TITLE;
    } else if (strcmp(name, "Subject") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_SUBJECT;
    } else if (strcmp(name, "Creator") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_CREATOR;
    } else if (strcmp(name, "Producer") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_PRODUCER;
    } else if (strcmp(name, "CreationDate") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_CREATION_DATE;
    } else if (strcmp(name, "ModDate") == 0) {
      type = ZATHURA_DOCUMENT_INFORMATION_MODIFICATION_DATE;
    }

    zathura_document_information_entry_t* entry = zathura_document_information_entry_new(type, value);
    if (entry != NULL) {
      girara_list_append(list, entry);
    }
  }

  return list;
}

zathura_image_buffer_t*
pdf_page_render(zathura_page_t* page, mupdf_page_t* mupdf_page, zathura_error_t* error)
{
  if (page == NULL || mupdf_page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return NULL;
  }

  /* calculate sizes */
  double scale             = zathura_document_get_scale(document);
  unsigned int page_width  = scale * zathura_page_get_width(page);
  unsigned int page_height = scale * zathura_page_get_height(page);

  /* create image buffer */
  zathura_image_buffer_t* image_buffer = zathura_image_buffer_create(page_width, page_height);

  if (image_buffer == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  /* render */
  fz_display_list* display_list = fz_new_display_list();
  fz_device* device             = fz_new_list_device(display_list);

  if (pdf_run_page(mupdf_document->document, mupdf_page->page, device, fz_scale(scale, scale)) != fz_okay) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  fz_free_device(device);

  fz_bbox bbox = { .x1 = page_width, .y1 = page_height };

  fz_pixmap* pixmap = fz_new_pixmap_with_rect(fz_device_rgb, bbox);
  fz_clear_pixmap_with_color(pixmap, 0xFF);

  device = fz_new_draw_device(mupdf_document->glyph_cache, pixmap);
  fz_execute_display_list(display_list, device, fz_identity, bbox);
  fz_free_device(device);

  unsigned char* s = pixmap->samples;
  for (unsigned int y = 0; y < pixmap->h; y++) {
    for (unsigned int x = 0; x < pixmap->w; x++) {
      guchar* p = image_buffer->data + (pixmap->h - y - 1) *
        image_buffer->rowstride + x * 3;
      p[0] = s[2];
      p[1] = s[1];
      p[2] = s[0];
      s += pixmap->n;
    }
  }

  fz_drop_pixmap(pixmap);
  fz_free_display_list(display_list);

  return image_buffer;
}

#if HAVE_CAIRO
zathura_error_t
pdf_page_render_cairo(zathura_page_t* page, mupdf_page_t* mupdf_page, cairo_t* cairo, bool GIRARA_UNUSED(printing))
{
  if (page == NULL || mupdf_page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  cairo_surface_t* surface = cairo_get_target(cairo);
  if (surface == NULL) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  unsigned int page_width  = cairo_image_surface_get_width(surface);
  unsigned int page_height = cairo_image_surface_get_height(surface);

  double scalex = ((double) page_width) / zathura_page_get_width(page);
  double scaley = ((double) page_height) /zathura_page_get_height(page);

  /* render */
  fz_display_list* display_list = fz_new_display_list();
  fz_device* device             = fz_new_list_device(display_list);

  if (pdf_run_page(mupdf_document->document, mupdf_page->page, device, fz_scale(scalex, scaley)) != fz_okay) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  fz_free_device(device);


  fz_bbox bbox = { .x1 = page_width, .y1 = page_height };

  fz_pixmap* pixmap = fz_new_pixmap_with_rect(fz_device_rgb, bbox);
  fz_clear_pixmap_with_color(pixmap, 0xFF);

  device = fz_new_draw_device(mupdf_document->glyph_cache, pixmap);
  fz_execute_display_list(display_list, device, fz_identity, bbox);
  fz_free_device(device);

  int rowstride        = cairo_image_surface_get_stride(surface);
  unsigned char* image = cairo_image_surface_get_data(surface);

  unsigned char *s = pixmap->samples;
  for (unsigned int y = 0; y < pixmap->h; y++) {
    for (unsigned int x = 0; x < pixmap->w; x++) {
      guchar* p = image + (pixmap->h - y - 1) * rowstride + x * 4;
      p[0] = s[2];
      p[1] = s[1];
      p[2] = s[0];
      s += pixmap->n;
    }
  }

  fz_drop_pixmap(pixmap);
  fz_free_display_list(display_list);

  return ZATHURA_ERROR_OK;;
}
#endif

static inline int
text_span_char_at(fz_text_span *span, int index)
{
  int offset = 0;
  while (span != NULL) {
    if (index < offset + span->len) {
      return span->text[index - offset].c;
    }

    if (span->eol != 0) {
      if (index == offset + span->len) {
        return ' ';
      }
      offset++;
    }

    offset += span->len;
    span = span->next;
  }

  return 0;
}

static unsigned int
text_span_length(fz_text_span *span)
{
  unsigned int length = 0;

  while (span != NULL) {
    length += span->len;

    if (span->eol != 0) {
      length++;
    }

    span = span->next;
  }

  return length;
}

static int
text_span_match_string_n(fz_text_span* span, const char* string, int n,
    zathura_rectangle_t* rectangle)
{
  if (span == NULL || string == NULL || rectangle == NULL) {
    return 0;
  }

  int o = n;
  int c;

  while ((c = *string++)) {
    if (c == ' ' && text_span_char_at(span, n) == ' ') {
      while (text_span_char_at(span, n) == ' ') {
        search_result_add_char(rectangle, span, n);
        n++;
      }
    } else {
      if (tolower(c) != tolower(text_span_char_at(span, n))) {
        return 0;
      }
      search_result_add_char(rectangle, span, n);
      n++;
    }
  }

  return n - o;
}

static void
pdf_zathura_image_free(zathura_image_t* image)
{
  if (image == NULL) {
    return;
  }

  g_free(image);
}

static void
get_images(fz_obj* dict, girara_list_t* list)
{
  if (dict == NULL || list == NULL) {
    return;
  }

  for (int i = 0; i < fz_dict_len(dict); i++) {
    fz_obj* image_dict = fz_dict_get_val(dict, i);
    if (fz_is_dict(image_dict) == 0) {
      continue;
    }

    fz_obj* type = fz_dict_gets(image_dict, "Subtype");
    if (strcmp(fz_to_name(type), "Image") != 0) {
      continue;
    }

    bool duplicate = false;
    GIRARA_LIST_FOREACH(list, zathura_image_t*, iter, image)
      if (image->data == image_dict) {
        duplicate = true;
        break;
      }
    GIRARA_LIST_FOREACH_END(list, zathura_image_t*, iter, image);

    if (duplicate == true) {
      continue;
    }

    fz_obj* width  = fz_dict_gets(image_dict, "Width");
    fz_obj* height = fz_dict_gets(image_dict, "Height");

    zathura_image_t* zathura_image = g_malloc(sizeof(zathura_image_t));

    // FIXME: Get correct image coordinates
    zathura_image->data        = image_dict;
    zathura_image->position.x1 = 0;
    zathura_image->position.x2 = fz_to_int(width);
    zathura_image->position.y1 = 0;
    zathura_image->position.y2 = fz_to_int(height);

    girara_list_append(list, zathura_image);
  }
}

static void
get_resources(fz_obj* resource, girara_list_t* list)
{
  if (resource == NULL || list == NULL) {
    return;
  }

  fz_obj* x_object = fz_dict_gets(resource, "XObject");
  if (x_object == NULL) {
    return;
  }

  get_images(x_object, list);

  for (int i = 0; i < fz_dict_len(x_object); i++) {
    fz_obj* obj = fz_dict_get_val(x_object, i);
    fz_obj* subsrc = fz_dict_gets(obj, "Resources");
    if (subsrc != NULL && fz_objcmp(resource, subsrc)) {
      get_resources(subsrc, list);
    }
  }
}

static void
search_result_add_char(zathura_rectangle_t* rectangle, fz_text_span* span,
    int index)
{
  if (rectangle == NULL || span == NULL) {
    return;
  }

  int offset = 0;
  for (; span != NULL; span = span->next) {
    if (index < offset + span->len) {
      fz_bbox coordinates = span->text[index - offset].bbox;

      if (rectangle->x1 == 0) {
        rectangle->x1 = coordinates.x0;
      }

      if (coordinates.x1 > rectangle->x2) {
        rectangle->x2 = coordinates.x1;
      }

      if (coordinates.y1 > rectangle->y1) {
        rectangle->y1 = coordinates.y1;
      }

      if (rectangle->y2 == 0) {
        rectangle->y2 = coordinates.y0;
      }

      return;
    }

    if (span->eol != 0) {
      offset++;
    }

    offset += span->len;
  }
}

static void
mupdf_page_extract_text(pdf_xref* document, mupdf_page_t* mupdf_page)
{
  if (document == NULL || mupdf_page == NULL || mupdf_page->extracted_text == true) {
    return;
  }

  fz_display_list* display_list = fz_new_display_list();
  fz_device* device             = fz_new_list_device(display_list);
  fz_device* text_device        = fz_new_text_device(mupdf_page->text);

  if (pdf_run_page(document, mupdf_page->page, device, fz_identity) != fz_okay) {
    return;
  }

  fz_execute_display_list(display_list, text_device, fz_identity, fz_infinite_bbox);
  mupdf_page->extracted_text = true;

  fz_free_device(text_device);
  fz_free_device(device);
  fz_free_display_list(display_list);
}

static void
build_index(mupdf_document_t* mupdf_document, pdf_outline* outline, girara_tree_node_t* root)
{
  if (mupdf_document == NULL || outline == NULL || root == NULL) {
    return;
  }

  while (outline != NULL) {
    zathura_index_element_t* index_element = zathura_index_element_new(outline->title);
    zathura_link_target_t target;
    zathura_link_type_t type;
    zathura_rectangle_t rect;
    char* buffer = NULL;

    switch (outline->link->kind) {
      case PDF_LINK_URI:
        /* extract uri */
        buffer = g_malloc0(sizeof(char) * (fz_to_str_len(outline->link->dest) + 1));
        memcpy(buffer, fz_to_str_buf(outline->link->dest), fz_to_str_len(outline->link->dest));
        buffer[fz_to_str_len(outline->link->dest)] = '\0';
        target.value = buffer;

        type = ZATHURA_LINK_URI;
        break;
      case  PDF_LINK_GOTO:
        target.page_number = pdf_find_page_number(mupdf_document->document,
            fz_array_get(outline->link->dest, 0));

        type = ZATHURA_LINK_GOTO_DEST;
        break;
      default:
        continue;
    }

    index_element->link = zathura_link_new(type, rect, target);
    if (index_element->link == NULL) {
      continue;
    }

    girara_tree_node_t* node = girara_node_append_data(root, index_element);

    if (outline->child != NULL) {
      build_index(mupdf_document, outline->child, node);
    }

    outline = outline->next;
  }
}
