/*================================================================================

    csmi/src/wm/src/csmi_soft_failure_recovery.c

  © Copyright IBM Corporation 2015-2018. All Rights Reserved

    This program is licensed under the terms of the Eclipse Public License
    v1.0 as published by the Eclipse Foundation and available at
    http://www.eclipse.org/legal/epl-v10.html

    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
    restricted by GSA ADP Schedule Contract with IBM Corp.

================================================================================*/
#include "csmutil/include/csmutil_logging.h"
#include "csmutil/include/timing.h"

#include "csmi/src/common/include/csmi_api_internal.h"
#include "csmi/src/common/include/csmi_common_utils.h"
#include "csmi/include/csm_api_workload_manager.h"
#include <string.h>
#include <ctype.h>

// The structs used in sending and recieving queries from the infrastructure.
#define API_PARAMETER_INPUT_TYPE csm_soft_failure_recovery_input_t
#define API_PARAMETER_OUTPUT_TYPE csm_soft_failure_recovery_output_t

static const csmi_cmd_t expected_cmd = CSM_CMD_soft_failure_recovery;

void csmi_soft_failure_recovery_destroy(csm_api_object *handle);

int csm_soft_failure_recovery(
    csm_api_object **handle,
    API_PARAMETER_INPUT_TYPE *input,
    API_PARAMETER_OUTPUT_TYPE **output)
{
    START_TIMING()

    // Declare variables that we will use below.
    char    *buffer             = NULL;
    uint32_t buffer_length      = 0;   
    char     *return_buffer     = NULL; 
    uint32_t  return_buffer_len = 0;   
    int      error_code         = CSMI_SUCCESS;

    // EARLY RETURN
    // Create a csm_api_object and sets its csmi cmd and the destroy function
    create_csm_api_object(handle, expected_cmd, csmi_soft_failure_recovery_destroy);
    
    // Send a Message to the Backend.
    error_code = csmi_sendrecv_cmd(*handle, expected_cmd, 
        buffer, buffer_length, &return_buffer, &return_buffer_len);

    // If a success was detected process it, otherwise log the failure.
    if ( error_code == CSMI_SUCCESS )
    {
         if ( return_buffer )
         {
             if ( csm_deserialize_struct(API_PARAMETER_OUTPUT_TYPE, output,
                     (const char *)return_buffer, return_buffer_len) == 0 )
             {
                 csm_api_object_set_retdata(*handle, 1, *output);
             }
             else
             {
                 csmutil_logging(error, "Deserialization failed");
                 csm_api_object_errcode_set(*handle, CSMERR_MSG_UNPACK_ERROR);
                 csm_api_object_errmsg_set(*handle,
                     strdup(csm_get_string_from_enum(csmi_cmd_err_t, CSMERR_MSG_UNPACK_ERROR)));
                 error_code = CSMERR_MSG_UNPACK_ERROR;
             }
         }
         else
         {
             csm_api_object_errcode_set(*handle, CSMI_NO_RESULTS);
             error_code = CSMI_NO_RESULTS;
         } 
    }    
    else
    {
        csmutil_logging(error, "csmi_sendrecv_cmd failed: %d - %s",
            error_code, csm_api_object_errmsg_get(*handle));
    }

    free(buffer);
 
    END_TIMING( csmapi, trace, csm_api_object_traceid_get(*handle), expected_cmd, api )

    return error_code;
}

void csmi_soft_failure_recovery_destroy(csm_api_object *handle)
{
    csmi_api_internal *csmi_hdl;
    API_PARAMETER_OUTPUT_TYPE *sfr;

    // free the CSMI dependent data
    csmi_hdl = (csmi_api_internal *) handle->hdl;
    if (csmi_hdl->cmd != expected_cmd)
    {
        csmutil_logging(error, "%s-%d: Unmatched CSMI cmd\n", __FILE__, __LINE__);
        return;
    }

    // free the returned data specific to this csmi cmd
    sfr = (API_PARAMETER_OUTPUT_TYPE *) csmi_hdl->ret_cdata;
    csm_free_struct_ptr( API_PARAMETER_OUTPUT_TYPE, sfr);

    csmutil_logging(info, "csmi_allocation_query_destroy called");
}

