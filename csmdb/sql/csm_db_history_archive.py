#!/bin/python
# encoding: utf-8
#================================================================================
#   
#    csm_db_history_archive.py
# 
#    © Copyright IBM Corporation 2015-2018. All Rights Reserved
#
#    This program is licensed under the terms of the Eclipse Public License
#    v1.0 as published by the Eclipse Foundation and available at
#    http://www.eclipse.org/legal/epl-v10.html
#
#    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
#    restricted by GSA ADP Schedule Contract with IBM Corp.
# 
#================================================================================

import psycopg2
import argparse
import json
import sys
import os
import logging.config
import logging.handlers as handlers
import time
from logging.handlers import RotatingFileHandler
import datetime
from datetime import date
import threading
from multiprocessing.dummy import Pool as ThreadPool

DEFAULT_LOG='''/var/log/ibm/csm/db/'''
DEFAULT_TARGET='''/var/log/ibm/csm/archive'''
DEFAULT_COUNT=1000
DEFAULT_DATABASE="csmdb"
DEFAULT_USER="postgres"
DEFAULT_THREAD_POOL=2

logger = logging.getLogger()
logger.setLevel(logging.DEBUG)

# Absolute path to prevent file rotation issues.
logfile = os.path.abspath("csm_db_archive_script.log")

# Add a rotating handler
handler = logging.handlers.RotatingFileHandler('csm_db_archive_script.log',mode='w',maxBytes=1000000,backupCount=365)

# Create and configure logger
LOG_FORMAT = logging.Formatter('%(asctime)s (%(name)s) [%(levelname)s]:\t%(message)s')

# Add handler and formatter.
handler.setFormatter(LOG_FORMAT)
logger.addHandler(handler)

# Test the logger
a = datetime.datetime.now()
print "---------------------------------------------------------------------------------------------------------"
print "Welcome to the CSM DB archiving script"
print "---------------------------------------------------------------------------------------------------------"
logger.info("Welcome to the CSM DB archiving script")
logger.info("---------------------------------------------------------------------------------------------------------")
logging.info("Start Script Time:                 | {0}".format(a))
logger.info("---------------------------------------------------------------------------------------------------------")

TABLES=[ "csm_allocation_history", "csm_allocation_node_history", "csm_allocation_state_history", 
    "csm_config_history", "csm_db_schema_version_history", "csm_diag_result_history", 
    "csm_diag_run_history", "csm_dimm_history", "csm_gpu_history", "csm_hca_history", 
    "csm_ib_cable_history", "csm_lv_history", "csm_lv_update_history", "csm_node_history", 
    "csm_node_state_history", "csm_processor_socket_history", "csm_ssd_history", 
    "csm_ssd_wear_history", "csm_step_history", "csm_step_node_history", "csm_switch_history",
    "csm_switch_inventory_history", "csm_vg_history", "csm_vg_ssd_history"]

RAS_TABLES=["csm_ras_event_action"]

def dump_table( db, user, table_name, count, target_dir, is_ras=False ):
    try:
        db_conn= psycopg2.connect("dbname='{0}' user='{1}' host='localhost'".format(db, user))
    except:
        print "[CRITICAL] Unable to connect to local database."
        logger.info("Unable to connect to local database.")
        return
    print "[INFO] Processing {0}".format(table_name)

    time= "master_time_stamp" if is_ras else "history_time"

    cursor=db_conn.cursor()
    
    cursor.execute("DROP TABLE IF EXISTS temp_{0}".format(table_name))

    ## Make the temp table.
    cursor.execute("CREATE TEMP TABLE temp_{0} as \
        ( SELECT *, ctid AS id \
          FROM {0} WHERE archive_history_time IS NULL \
          ORDER BY {1} ASC LIMIT {2} FOR UPDATE)".format(table_name, time, count))

    ## Select the count.
    sql = "SELECT count(*) FROM temp_{0}".format(table_name)
    number_of_rows = cursor.execute(sql)
    (result,) = cursor.fetchall()
    
    ## Update the flagged tables.
    cursor.execute("UPDATE {0} SET archive_history_time = 'now()' \
        FROM temp_{0} WHERE temp_{0}.id = {0}.ctid \
        AND {0}.archive_history_time IS NULL \
        AND {0}.{1} = temp_{0}.{1}".format(table_name,time))
    
    ## Drop the id from the temp table.
    cursor.execute("ALTER TABLE temp_{0} DROP COLUMN id".format(table_name))

    ## Dump the updated table.
    cursor.execute("SELECT * FROM temp_{0}".format(table_name))

    # Build the file name.
    file_name="{0}/{1}.archive.{2}.json".format(target_dir, table_name, date.today())

    #cursor.execute( "UPDATE {0} SET archive_history_time = 'now()' \
    #    FROM {0} as t\
    #    INNER JOIN \
    #        (SELECT *,ctid FROM {0} WHERE archive_history_time IS NULL \
    #        ORDER BY {1} ASC LIMIT {2})  as filter\
    #        ON filter.ctid = t.ctid \
    #    RETURNING row_to_json({0})".format(table_name, time, count)) 
    
    # Append the logs to the file.
    try:
        with open(file_name, 'a') as file:

            colnames = [desc[0] for desc in cursor.description]
            for row in cursor:
                file.write('{{ "type":"db-{0}", "data":{1} }}\n'.format(
                    table_name, json.dumps(dict(zip(colnames, row)), default=str)))
    except Exception as e:
        print "Exception caught: {0}".format(e)
        cursor.execute("DROP TABLE IF EXISTS temp_{0}".format(table_name))
        db_conn.rollback()
        db_conn.close()
        return
    
    # Commit the records.
    cursor.execute("DROP TABLE IF EXISTS temp_{0}".format(table_name))
    b = datetime.datetime.now()
    delta = b - a
    logger.info("Tbl: {0:<29} | User Ct: {1:<10} Act DB Ct: {2:<10}".format(table_name, count, result[0]))
    db_conn.commit()
    db_conn.close()

def main(args):

    # Parse the args.
    parser = argparse.ArgumentParser(
        description='''A tool for archiving the CSM Database history tables.''')
    
    parser.add_argument( '-t', '--target', metavar='dir', dest='target', default=DEFAULT_TARGET,
        help="Target directory to write archive and logging to. Default: {0}".format(DEFAULT_TARGET))
    parser.add_argument( '-n', '--count', metavar='count', dest="count", default=DEFAULT_COUNT,
        help="Number of records to archive in the run. Default: {0}".format(DEFAULT_COUNT))
    parser.add_argument( '-d', '--database', metavar='db', dest="db", default=DEFAULT_DATABASE,
        help="Database to archive tables from. Default: {0}".format(DEFAULT_DATABASE))
    parser.add_argument( '-u', '--user', metavar='user', dest="user", default=DEFAULT_USER,
        help="The database user. Default: {0}".format(DEFAULT_USER))
    parser.add_argument( '--threads', metavar='threads', dest="threads", default=DEFAULT_THREAD_POOL,
        help="The number of threads for the thread pool. Default: {0}".format(DEFAULT_THREAD_POOL))

    args = parser.parse_args()

    # Verifies path exists.
    if not os.path.exists(args.target):
        os.makedirs(args.target)
    
    pool = ThreadPool(int(args.threads))

    pool.map( lambda table: dump_table( args.db, args.user, table, args.count, args.target), TABLES)

    for table in RAS_TABLES:
        dump_table( args.db, args.user, table, args.count, args.target, True)

    # Process the finishing info.
    ft = datetime.datetime.now()
    delta2 = ft - a
    logging.info("---------------------------------------------------------------------------------------------------------")
    logging.info("Archiving Data Directory:          | {0}".format(args.target))
    logging.info("End Script Time:                   | {0}".format(ft))
    logging.info("Total Process Time:                | {0}".format(delta2))
    logging.info("---------------------------------------------------------------------------------------------------------")
    logging.info("Finish CSM DB archive script process")
    logging.info("---------------------------------------------------------------------------------------------------------")
    print "---------------------------------------------------------------------------------------------------------"

if __name__ == "__main__":
    sys.exit(main(sys.argv))
