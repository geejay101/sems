module "http_client" {
    # resend_interval=<msec>
    #
    # interval between resend attempts for all destinations with failed_upload action 'requeue'
    #
    # Default: 5000
    resend_interval=5000

    # resend_queue_max=<integer>
    #
    # maximum events queue size for all requeued uploads
    # set 0 for unlimited
    #
    # Default: 10000
    resend_queue_max=0

    # destination "<destination name>
    destination "pcap" {
        # mode=<modes>
        #
        # available modes: put, post
        #
        # Default: mandatory
        mode=put
        # url={url[,url2...]}
        #
        # comma-spearated list of URLs
        # automatic failover if specified more than one url
        #
        # Default: mandatory
        url={http://storage.pbx2-sandbox.didww.com:6666/pcap-dumps/}
        # content_type=<mimetype>
        #
        # supported only in 'post' mode
        # specifies custom Content-Type http header value
        #
        # Example value: application/vnd.api+json

        # Default: empty
        #
        # succ_codes={mask[,mask2...]}
        #
        # comma-spearated list of the masks
        # for codes to be considered as successfull
        #
        # Default: 2xx
        #succ_codes={2xx}

        # action "<action name>"
        #
        # available names: success, fail
        action "success" {
            # value=<action value>
            #
            # available actions: remove, move, nothing
            #
            # Default: remove
            value=remove

            # args=<args>
            #
            # meaning depends from choosen post-upload action
            #
            # Default: mandatory for 'move' post-upload action
            #
            # args=/tmp
        }
        action "fail" {
            # value=<action value>
            #
            # available actions: remove, move, nothing, requeue
            #
            # Default: requeue
            value=requeue

            # args=<args>
            #
            # meaning depends from choosen post-upload action
            #
            # Default: mandatory for 'move' post-upload action
            #
            # args=/tmp
        }
    }
    # requeue_limit=<integer>
    #
    # Limit of the attempts for requeue fail action
    # unlimited if zero
    #
    # Default: 0
    # pcap_requeue_limit=0
}
