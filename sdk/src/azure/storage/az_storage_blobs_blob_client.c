// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/core/az_http.h>
#include <azure/core/az_http_transport.h>
#include <azure/core/az_json.h>
#include <azure/core/az_precondition.h>
#include <azure/core/internal/az_config_internal.h>
#include <azure/core/internal/az_credentials_internal.h>
#include <azure/core/internal/az_http_internal.h>
#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/internal/az_result_internal.h>
#include <azure/core/internal/az_span_internal.h>
#include <azure/storage/az_storage_blobs.h>

#include <stddef.h>

#include <azure/core/_az_cfg.h>

enum
{
  _az_STORAGE_HTTP_REQUEST_HEADER_BUFFER_SIZE = 10 * sizeof(_az_http_request_header),
};

static az_span const AZ_STORAGE_BLOBS_BLOB_HEADER_X_MS_BLOB_TYPE
    = AZ_SPAN_LITERAL_FROM_STR("x-ms-blob-type");

static az_span const AZ_STORAGE_BLOBS_BLOB_TYPE_BLOCKBLOB = AZ_SPAN_LITERAL_FROM_STR("BlockBlob");

static az_span const AZ_HTTP_HEADER_CONTENT_LENGTH = AZ_SPAN_LITERAL_FROM_STR("Content-Length");
static az_span const AZ_HTTP_HEADER_CONTENT_TYPE = AZ_SPAN_LITERAL_FROM_STR("Content-Type");

AZ_NODISCARD az_storage_blobs_blob_client_options az_storage_blobs_blob_client_options_default()
{

  az_storage_blobs_blob_client_options options = (az_storage_blobs_blob_client_options) {
    ._internal = {
      .api_version = { 
        ._internal = { 
          .option_location = _az_http_policy_apiversion_option_location_header,
          .name = AZ_SPAN_FROM_STR("x-ms-version"),
          .version = AZ_STORAGE_API_VERSION,
        },
      },
      .telemetry_options = _az_http_policy_telemetry_options_default(),
    },
    .retry_options = _az_http_policy_retry_options_default(),
  };

  options.retry_options.max_retries = 5;
  options.retry_options.retry_delay_msec = 1 * _az_TIME_MILLISECONDS_PER_SECOND;
  options.retry_options.max_retry_delay_msec = 30 * _az_TIME_MILLISECONDS_PER_SECOND;

  return options;
}

AZ_NODISCARD az_result az_storage_blobs_blob_client_init(
    az_storage_blobs_blob_client* out_client,
    az_span endpoint,
    void* credential,
    az_storage_blobs_blob_client_options const* options)
{
  _az_PRECONDITION_NOT_NULL(out_client);
  _az_PRECONDITION_NOT_NULL(options);

  _az_credential* const cred = (_az_credential*)credential;

  *out_client = (az_storage_blobs_blob_client) {
    ._internal = {
      .endpoint = AZ_SPAN_FROM_BUFFER(out_client->_internal.endpoint_buffer),
      .options = *options,
      .credential = cred,
      .pipeline = (_az_http_pipeline){
        ._internal = {
          .policies = {
            {
              ._internal = {
                .process = az_http_pipeline_policy_apiversion,
                .options= &out_client->_internal.options._internal.api_version,
              },
            },
            {
              ._internal = {
                .process = az_http_pipeline_policy_telemetry,
                .options = &out_client->_internal.options._internal.telemetry_options,
              },
            },
            {
              ._internal = {
                .process = az_http_pipeline_policy_retry,
                .options = &out_client->_internal.options.retry_options,
              },
            },
            {
              ._internal = {
                .process = az_http_pipeline_policy_credential,
                .options = cred,
              },
            },
#ifndef AZ_NO_LOGGING
            {
              ._internal = {
                .process = az_http_pipeline_policy_logging,
                .options = NULL,
              },
            },
#endif // AZ_NO_LOGGING
            {
              ._internal = {
                .process = az_http_pipeline_policy_transport,
                .options = NULL,
              },
            },
          },
        }
      }
    }
  };

  // Copy url to client buffer so customer can re-use buffer on his/her side
  int32_t const uri_size = az_span_size(endpoint);
  _az_RETURN_IF_NOT_ENOUGH_SIZE(out_client->_internal.endpoint, uri_size);
  az_span_copy(out_client->_internal.endpoint, endpoint);
  out_client->_internal.endpoint = az_span_slice(out_client->_internal.endpoint, 0, uri_size);

  _az_RETURN_IF_FAILED(
      _az_credential_set_scopes(cred, AZ_SPAN_FROM_STR("https://storage.azure.com/.default")));

  return AZ_OK;
}

AZ_NODISCARD az_result az_storage_blobs_blob_upload(
    az_storage_blobs_blob_client* ref_client,
    az_span content, /* Buffer of content*/
    az_storage_blobs_blob_upload_options const* options,
    az_http_response* ref_response)
{

  az_storage_blobs_blob_upload_options opt;
  if (options == NULL)
  {
    opt = az_storage_blobs_blob_upload_options_default();
  }
  else
  {
    opt = *options;
  }

  // Request buffer
  // create request buffer TODO: define size for a blob upload
  uint8_t url_buffer[AZ_HTTP_REQUEST_URL_BUFFER_SIZE];
  az_span request_url_span = AZ_SPAN_FROM_BUFFER(url_buffer);
  // copy url from client
  int32_t uri_size = az_span_size(ref_client->_internal.endpoint);
  _az_RETURN_IF_NOT_ENOUGH_SIZE(request_url_span, uri_size);
  az_span_copy(request_url_span, ref_client->_internal.endpoint);

  uint8_t headers_buffer[_az_STORAGE_HTTP_REQUEST_HEADER_BUFFER_SIZE];
  az_span request_headers_span = AZ_SPAN_FROM_BUFFER(headers_buffer);

  // create request
  az_http_request request;
  _az_RETURN_IF_FAILED(az_http_request_init(
      &request,
      opt.context,
      az_http_method_put(),
      request_url_span,
      uri_size,
      request_headers_span,
      content));

  // add blob type to request
  _az_RETURN_IF_FAILED(az_http_request_append_header(
      &request, AZ_STORAGE_BLOBS_BLOB_HEADER_X_MS_BLOB_TYPE, AZ_STORAGE_BLOBS_BLOB_TYPE_BLOCKBLOB));

  uint8_t content_length[_az_INT64_AS_STR_BUFFER_SIZE] = { 0 };
  az_span content_length_span = AZ_SPAN_FROM_BUFFER(content_length);
  az_span remainder;
  _az_RETURN_IF_FAILED(az_span_i64toa(content_length_span, az_span_size(content), &remainder));
  content_length_span
      = az_span_slice(content_length_span, 0, _az_span_diff(remainder, content_length_span));

  // add Content-Length to request
  _az_RETURN_IF_FAILED(
      az_http_request_append_header(&request, AZ_HTTP_HEADER_CONTENT_LENGTH, content_length_span));

  // add blob type to request
  _az_RETURN_IF_FAILED(az_http_request_append_header(
      &request, AZ_HTTP_HEADER_CONTENT_TYPE, AZ_SPAN_FROM_STR("text/plain")));

  // start pipeline
  return az_http_pipeline_process(&ref_client->_internal.pipeline, &request, ref_response);
}
